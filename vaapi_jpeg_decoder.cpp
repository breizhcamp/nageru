#include "vaapi_jpeg_decoder.h"

#include "jpeg_destroyer.h"
#include "jpeg_frame.h"
#include "memcpy_interleaved.h"

#include <X11/Xlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <jpeglib.h>
#include <list>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_x11.h>

using namespace std;

static unique_ptr<VADisplayWithCleanup> va_dpy;
static VAConfigID config_id;
static VAImageFormat uyvy_format;
bool vaapi_jpeg_decoding_usable = false;

struct VAResources {
	unsigned width, height;
	VASurfaceID surface;
	VAContextID context;
	VAImage image;
};
static list<VAResources> va_resources_freelist;
static mutex va_resources_mutex;

#define CHECK_VASTATUS(va_status, func)                                 \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr, "%s:%d (%s) failed with %d\n", __func__, __LINE__, func, va_status); \
        exit(1);                                                        \
    }

#define CHECK_VASTATUS_RET(va_status, func)                             \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr, "%s:%d (%s) failed with %d\n", __func__, __LINE__, func, va_status); \
        return nullptr;                                                 \
    }

VAResources get_va_resources(unsigned width, unsigned height)
{
	{
		lock_guard<mutex> lock(va_resources_mutex);
		for (auto it = va_resources_freelist.begin(); it != va_resources_freelist.end(); ++it) {
			if (it->width == width && it->height == height) {
				VAResources ret = *it;
				va_resources_freelist.erase(it);
				return ret;
			}
		}
	}

	VAResources ret;

	ret.width = width;
	ret.height = height;

	VAStatus va_status = vaCreateSurfaces(va_dpy->va_dpy, VA_RT_FORMAT_YUV422,
		width, height,
		&ret.surface, 1, nullptr, 0);
	CHECK_VASTATUS(va_status, "vaCreateSurfaces");

	va_status = vaCreateContext(va_dpy->va_dpy, config_id, width, height, 0, &ret.surface, 1, &ret.context);
	CHECK_VASTATUS(va_status, "vaCreateContext");

	va_status = vaCreateImage(va_dpy->va_dpy, &uyvy_format, width, height, &ret.image);
	CHECK_VASTATUS(va_status, "vaCreateImage");

	return ret;
}

void release_va_resources(VAResources resources)
{
	lock_guard<mutex> lock(va_resources_mutex);
	if (va_resources_freelist.size() > 10) {
		auto it = va_resources_freelist.end();
		--it;

		VAStatus va_status = vaDestroyImage(va_dpy->va_dpy, it->image.image_id);
		CHECK_VASTATUS(va_status, "vaDestroyImage");

		va_status = vaDestroyContext(va_dpy->va_dpy, it->context);
		CHECK_VASTATUS(va_status, "vaDestroyContext");

		va_status = vaDestroySurfaces(va_dpy->va_dpy, &it->surface, 1);
		CHECK_VASTATUS(va_status, "vaDestroySurfaces");

		va_resources_freelist.erase(it);
	}

	va_resources_freelist.push_front(resources);
}

// RAII wrapper to release VAResources on return (even on error).
class ReleaseVAResources {
public:
	ReleaseVAResources(const VAResources &resources)
		: resources(resources) {}
	~ReleaseVAResources()
	{
		if (!committed) {
			release_va_resources(resources);
		}
	}

	void commit() { committed = true; }

private:
	const VAResources &resources;
	bool committed = false;
};

VADisplayWithCleanup::~VADisplayWithCleanup()
{
	if (va_dpy != nullptr) {
		vaTerminate(va_dpy);
	}
	if (x11_display != nullptr) {
		XCloseDisplay(x11_display);
	}
	if (drm_fd != -1) {
		close(drm_fd);
	}
}

unique_ptr<VADisplayWithCleanup> va_open_display(const string &va_display)
{
	if (va_display.empty() || va_display[0] != '/') {  // An X display.
		Display *x11_display = XOpenDisplay(va_display.empty() ? nullptr : va_display.c_str());
		if (x11_display == nullptr) {
			fprintf(stderr, "error: can't connect to X server!\n");
			return nullptr;
		}

		unique_ptr<VADisplayWithCleanup> ret(new VADisplayWithCleanup);
		ret->x11_display = x11_display;
		ret->va_dpy = vaGetDisplay(x11_display);
		if (ret->va_dpy == nullptr) {
			return nullptr;
		}
		return ret;
	} else {  // A DRM node on the filesystem (e.g. /dev/dri/renderD128).
		int drm_fd = open(va_display.c_str(), O_RDWR);
		if (drm_fd == -1) {
			perror(va_display.c_str());
			return nullptr;
		}
		unique_ptr<VADisplayWithCleanup> ret(new VADisplayWithCleanup);
		ret->drm_fd = drm_fd;
		ret->va_dpy = vaGetDisplayDRM(drm_fd);
		if (ret->va_dpy == nullptr) {
			return nullptr;
		}
		return ret;
	}
}

unique_ptr<VADisplayWithCleanup> try_open_va(const string &va_display, string *error)
{
	unique_ptr<VADisplayWithCleanup> va_dpy = va_open_display(va_display);
	if (va_dpy == nullptr) {
		if (error)
			*error = "Opening VA display failed";
		return nullptr;
	}
	int major_ver, minor_ver;
	VAStatus va_status = vaInitialize(va_dpy->va_dpy, &major_ver, &minor_ver);
	if (va_status != VA_STATUS_SUCCESS) {
		char buf[256];
		snprintf(buf, sizeof(buf), "vaInitialize() failed with status %d\n", va_status);
		if (error != nullptr)
			*error = buf;
		return nullptr;
	}

	int num_entrypoints = vaMaxNumEntrypoints(va_dpy->va_dpy);
	unique_ptr<VAEntrypoint[]> entrypoints(new VAEntrypoint[num_entrypoints]);
	if (entrypoints == nullptr) {
		if (error != nullptr)
			*error = "Failed to allocate memory for VA entry points";
		return nullptr;
	}

	vaQueryConfigEntrypoints(va_dpy->va_dpy, VAProfileJPEGBaseline, entrypoints.get(), &num_entrypoints);
	for (int slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
		if (entrypoints[slice_entrypoint] != VAEntrypointVLD) {
			continue;
		}

		// We found a usable decode, so return it.
		return va_dpy;
	}

	if (error != nullptr)
		*error = "Can't find VAEntrypointVLD for the JPEG profile";
	return nullptr;
}

string get_usable_va_display()
{
	// Reduce the amount of chatter while probing,
	// unless the user has specified otherwise.
	bool need_env_reset = false;
	if (getenv("LIBVA_MESSAGING_LEVEL") == nullptr) {
		setenv("LIBVA_MESSAGING_LEVEL", "0", true);
		need_env_reset = true;
	}

	// First try the default (ie., whatever $DISPLAY is set to).
	unique_ptr<VADisplayWithCleanup> va_dpy = try_open_va("", nullptr);
	if (va_dpy != nullptr) {
		if (need_env_reset) {
			unsetenv("LIBVA_MESSAGING_LEVEL");
		}
		return "";
	}

	fprintf(stderr, "The X11 display did not expose a VA-API JPEG decoder.\n");

	// Try all /dev/dri/render* in turn. TODO: Accept /dev/dri/card*, too?
	glob_t g;
	int err = glob("/dev/dri/renderD*", 0, nullptr, &g);
	if (err != 0) {
	        fprintf(stderr, "Couldn't list render nodes (%s) when trying to autodetect a replacement.\n", strerror(errno));
	} else {
		for (size_t i = 0; i < g.gl_pathc; ++i) {
			string path = g.gl_pathv[i];
			va_dpy = try_open_va(path, nullptr);
			if (va_dpy != nullptr) {
				fprintf(stderr, "Autodetected %s as a suitable replacement; using it.\n",
					path.c_str());
				globfree(&g);
				if (need_env_reset) {
					unsetenv("LIBVA_MESSAGING_LEVEL");
				}
				return path;
			}
		}
	}

	fprintf(stderr, "No suitable VA-API JPEG decoders were found in /dev/dri; giving up.\n");
	fprintf(stderr, "Note that if you are using an Intel CPU with an external GPU,\n");
	fprintf(stderr, "you may need to enable the integrated Intel GPU in your BIOS\n");
	fprintf(stderr, "to expose Quick Sync.\n");
	return "none";
}

void init_jpeg_vaapi()
{
	string dpy = get_usable_va_display();
	if (dpy == "none") {
		return;
	}

	va_dpy = try_open_va(dpy, nullptr);
	if (va_dpy == nullptr) {
		return;
	}

	VAConfigAttrib attr = { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV422 };

	VAStatus va_status = vaCreateConfig(va_dpy->va_dpy, VAProfileJPEGBaseline, VAEntrypointVLD,
		&attr, 1, &config_id);
	CHECK_VASTATUS(va_status, "vaCreateConfig");

	int num_formats = vaMaxNumImageFormats(va_dpy->va_dpy);
	assert(num_formats > 0);

	unique_ptr<VAImageFormat[]> formats(new VAImageFormat[num_formats]);
	va_status = vaQueryImageFormats(va_dpy->va_dpy, formats.get(), &num_formats);
	CHECK_VASTATUS(va_status, "vaQueryImageFormats");

	bool found = false;
	for (int i = 0; i < num_formats; ++i) {
		// Seemingly VA_FOURCC_422H is no good for vaGetImage(). :-/
		if (formats[i].fourcc == VA_FOURCC_UYVY) {
			memcpy(&uyvy_format, &formats[i], sizeof(VAImageFormat));
			found = true;
			break;
		}
	}
	if (!found) {
		return;
	}

	fprintf(stderr, "VA-API JPEG decoding initialized.\n");
	vaapi_jpeg_decoding_usable = true;
}

class VABufferDestroyer {
public:
	VABufferDestroyer(VADisplay dpy, VABufferID buf)
		: dpy(dpy), buf(buf) {}

	~VABufferDestroyer() {
		vaDestroyBuffer(dpy, buf);
	}

private:
	VADisplay dpy;
	VABufferID buf;
};

shared_ptr<Frame> decode_jpeg_vaapi(const string &filename)
{
	jpeg_decompress_struct dinfo;
	jpeg_error_mgr jerr;
	dinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&dinfo);
	JPEGDestroyer destroy_dinfo(&dinfo);

	FILE *fp = fopen(filename.c_str(), "rb");
	if (fp == nullptr) {
		perror(filename.c_str());
		exit(1);
	}
	jpeg_stdio_src(&dinfo, fp);

	jpeg_read_header(&dinfo, true);

	// Read the data that comes after the header. VA-API will destuff and all for us.
	std::string str((const char *)dinfo.src->next_input_byte, dinfo.src->bytes_in_buffer);
	while (!feof(fp)) {
		char buf[4096];
		size_t ret = fread(buf, 1, sizeof(buf), fp);
		str.append(buf, ret);
	}
	fclose(fp);

	if (dinfo.num_components != 3) {
		fprintf(stderr, "Not a color JPEG. (%d components, Y=%dx%d, Cb=%dx%d, Cr=%dx%d)\n",
			dinfo.num_components,
			dinfo.comp_info[0].h_samp_factor, dinfo.comp_info[0].v_samp_factor,
			dinfo.comp_info[1].h_samp_factor, dinfo.comp_info[1].v_samp_factor,
			dinfo.comp_info[2].h_samp_factor, dinfo.comp_info[2].v_samp_factor);
		return nullptr;
	}
	if (dinfo.comp_info[0].h_samp_factor != 2 ||
	    dinfo.comp_info[1].h_samp_factor != 1 ||
	    dinfo.comp_info[1].v_samp_factor != dinfo.comp_info[0].v_samp_factor ||
	    dinfo.comp_info[2].h_samp_factor != 1 ||
	    dinfo.comp_info[2].v_samp_factor != dinfo.comp_info[0].v_samp_factor) {
		fprintf(stderr, "Not 4:2:2. (Y=%dx%d, Cb=%dx%d, Cr=%dx%d)\n",
			dinfo.comp_info[0].h_samp_factor, dinfo.comp_info[0].v_samp_factor,
			dinfo.comp_info[1].h_samp_factor, dinfo.comp_info[1].v_samp_factor,
			dinfo.comp_info[2].h_samp_factor, dinfo.comp_info[2].v_samp_factor);
		return nullptr;
	}

	// Picture parameters.
	VAPictureParameterBufferJPEGBaseline pic_param;
	memset(&pic_param, 0, sizeof(pic_param));
	pic_param.picture_width = dinfo.image_width;
	pic_param.picture_height = dinfo.image_height;
	for (int component_idx = 0; component_idx < dinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &dinfo.comp_info[component_idx];
		pic_param.components[component_idx].component_id = comp->component_id;
		pic_param.components[component_idx].h_sampling_factor = comp->h_samp_factor;
		pic_param.components[component_idx].v_sampling_factor = comp->v_samp_factor;
		pic_param.components[component_idx].quantiser_table_selector = comp->quant_tbl_no;
	}
	pic_param.num_components = dinfo.num_components;
	pic_param.color_space = 0;  // YUV.
	pic_param.rotation = VA_ROTATION_NONE;

	VABufferID pic_param_buffer;
	VAStatus va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_pic_param(va_dpy->va_dpy, pic_param_buffer);

	// Quantization matrices.
	VAIQMatrixBufferJPEGBaseline iq;
	memset(&iq, 0, sizeof(iq));

	for (int quant_tbl_idx = 0; quant_tbl_idx < min(4, NUM_QUANT_TBLS); ++quant_tbl_idx) {
		const JQUANT_TBL *qtbl = dinfo.quant_tbl_ptrs[quant_tbl_idx];
		if (qtbl == nullptr) {
			iq.load_quantiser_table[quant_tbl_idx] = 0;
		} else {
			iq.load_quantiser_table[quant_tbl_idx] = 1;
			for (int i = 0; i < 64; ++i) {
				if (qtbl->quantval[i] > 255) {
					fprintf(stderr, "Baseline JPEG only!\n");
					return nullptr;
				}
				iq.quantiser_table[quant_tbl_idx][i] = qtbl->quantval[i];
			}
		}
	}

	VABufferID iq_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAIQMatrixBufferType, sizeof(iq), 1, &iq, &iq_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_iq(va_dpy->va_dpy, iq_buffer);

	// Huffman tables (arithmetic is not supported).
	VAHuffmanTableBufferJPEGBaseline huff;
	memset(&huff, 0, sizeof(huff));

	for (int huff_tbl_idx = 0; huff_tbl_idx < min(2, NUM_HUFF_TBLS); ++huff_tbl_idx) {
		const JHUFF_TBL *ac_hufftbl = dinfo.ac_huff_tbl_ptrs[huff_tbl_idx];
		const JHUFF_TBL *dc_hufftbl = dinfo.dc_huff_tbl_ptrs[huff_tbl_idx];
		if (ac_hufftbl == nullptr) {
			assert(dc_hufftbl == nullptr);
			huff.load_huffman_table[huff_tbl_idx] = 0;
		} else {
			assert(dc_hufftbl != nullptr);
			huff.load_huffman_table[huff_tbl_idx] = 1;

			for (int i = 0; i < 16; ++i) {
				huff.huffman_table[huff_tbl_idx].num_dc_codes[i] = dc_hufftbl->bits[i + 1];
			}
			for (int i = 0; i < 12; ++i) {
				huff.huffman_table[huff_tbl_idx].dc_values[i] = dc_hufftbl->huffval[i];
			}
			for (int i = 0; i < 16; ++i) {
				huff.huffman_table[huff_tbl_idx].num_ac_codes[i] = ac_hufftbl->bits[i + 1];
			}
			for (int i = 0; i < 162; ++i) {
				huff.huffman_table[huff_tbl_idx].ac_values[i] = ac_hufftbl->huffval[i];
			}
		}
	}

	VABufferID huff_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAHuffmanTableBufferType, sizeof(huff), 1, &huff, &huff_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_huff(va_dpy->va_dpy, huff_buffer);

	// Slice parameters (metadata about the slice).
	VASliceParameterBufferJPEGBaseline parms;
	memset(&parms, 0, sizeof(parms));
	parms.slice_data_size = str.size();
	parms.slice_data_offset = 0;
	parms.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
	parms.slice_horizontal_position = 0;
	parms.slice_vertical_position = 0;
	for (int component_idx = 0; component_idx < dinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &dinfo.comp_info[component_idx];
		parms.components[component_idx].component_selector = comp->component_id;
		parms.components[component_idx].dc_table_selector = comp->dc_tbl_no;
		parms.components[component_idx].ac_table_selector = comp->ac_tbl_no;
		if (parms.components[component_idx].dc_table_selector > 1 ||
		    parms.components[component_idx].ac_table_selector > 1) {
			fprintf(stderr, "Uses too many Huffman tables\n");
			return nullptr;
		}
	}
	parms.num_components = dinfo.num_components;
	parms.restart_interval = dinfo.restart_interval;
	int horiz_mcus = (dinfo.image_width + (DCTSIZE * 2) - 1) / (DCTSIZE * 2);
	int vert_mcus = (dinfo.image_height + DCTSIZE - 1) / DCTSIZE;
	parms.num_mcus = horiz_mcus * vert_mcus;

	VABufferID slice_param_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VASliceParameterBufferType, sizeof(parms), 1, &parms, &slice_param_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_slice_param(va_dpy->va_dpy, slice_param_buffer);

	// The actual data.
	VABufferID data_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VASliceDataBufferType, str.size(), 1, &str[0], &data_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_data(va_dpy->va_dpy, data_buffer);

	VAResources resources = get_va_resources(dinfo.image_width, dinfo.image_height);
	ReleaseVAResources release(resources);

	va_status = vaBeginPicture(va_dpy->va_dpy, resources.context, resources.surface);
	CHECK_VASTATUS_RET(va_status, "vaBeginPicture");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &pic_param_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(pic_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &iq_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(iq)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &huff_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(huff)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &slice_param_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(slice_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &data_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(data)");
	va_status = vaEndPicture(va_dpy->va_dpy, resources.context);
	CHECK_VASTATUS_RET(va_status, "vaEndPicture");

	// vaDeriveImage() works, but the resulting image seems to live in
	// uncached memory, which makes copying data out from it very, very slow.
	// Thanks to FFmpeg for the observation that you can vaGetImage() the
	// surface onto your own image (although then, it can't be planar, which
	// is unfortunate for us).
#if 0
	VAImage image;
	va_status = vaDeriveImage(va_dpy->va_dpy, surf, &image);
	CHECK_VASTATUS_RET(va_status, "vaDeriveImage");
#else
	va_status = vaSyncSurface(va_dpy->va_dpy, resources.surface);
	CHECK_VASTATUS_RET(va_status, "vaSyncSurface");

	va_status = vaGetImage(va_dpy->va_dpy, resources.surface, 0, 0, dinfo.image_width, dinfo.image_height, resources.image.image_id);
	CHECK_VASTATUS_RET(va_status, "vaGetImage");
#endif

	void *mapped;
	va_status = vaMapBuffer(va_dpy->va_dpy, resources.image.buf, &mapped);
	CHECK_VASTATUS_RET(va_status, "vaMapBuffer");

	shared_ptr<Frame> frame(new Frame);
#if 0
	// 4:2:2 planar (for vaDeriveImage).
	frame->y.reset(new uint8_t[dinfo.image_width * dinfo.image_height]);
	frame->cb.reset(new uint8_t[(dinfo.image_width / 2) * dinfo.image_height]);
	frame->cr.reset(new uint8_t[(dinfo.image_width / 2) * dinfo.image_height]);
	for (int component_idx = 0; component_idx < dinfo.num_components; ++component_idx) {
		uint8_t *dptr;
		size_t width;
		if (component_idx == 0) {
			dptr = frame->y.get();
			width = dinfo.image_width;
		} else if (component_idx == 1) {
			dptr = frame->cb.get();
			width = dinfo.image_width / 2;
		} else if (component_idx == 2) {
			dptr = frame->cr.get();
			width = dinfo.image_width / 2;
		} else {
			assert(false);
		}
		const uint8_t *sptr = (const uint8_t *)mapped + image.offsets[component_idx];
		size_t spitch = image.pitches[component_idx];
		for (size_t y = 0; y < dinfo.image_height; ++y) {
			memcpy(dptr + y * width, sptr + y * spitch, width);
		}
	}
#else
	// Convert Y'CbCr to separate Y' and CbCr.
	frame->is_semiplanar = true;
	frame->y.reset(new uint8_t[dinfo.image_width * dinfo.image_height]);
	frame->cbcr.reset(new uint8_t[dinfo.image_width * dinfo.image_height]);
	const uint8_t *src = (const uint8_t *)mapped + resources.image.offsets[0];
	if (resources.image.pitches[0] == dinfo.image_width * 2) {
		memcpy_interleaved(frame->cbcr.get(), frame->y.get(), src, dinfo.image_width * dinfo.image_height * 2);
	} else {
		for (unsigned y = 0; y < dinfo.image_height; ++y) {
			memcpy_interleaved(frame->cbcr.get() + y * dinfo.image_width, frame->y.get() + y * dinfo.image_width,
			                   src + y * resources.image.pitches[0], dinfo.image_width * 2);
		}
	}
#endif
	frame->width = dinfo.image_width;
	frame->height = dinfo.image_height;
	frame->chroma_subsampling_x = 2;
	frame->chroma_subsampling_y = 1;
	frame->pitch_y = dinfo.image_width;
	frame->pitch_chroma = dinfo.image_width / 2;

	va_status = vaUnmapBuffer(va_dpy->va_dpy, resources.image.buf);
	CHECK_VASTATUS_RET(va_status, "vaUnmapBuffer");

	return frame;
}
