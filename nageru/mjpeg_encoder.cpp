#include "mjpeg_encoder.h"

#include <jpeglib.h>
#include <unistd.h>
#if __SSE2__
#include <immintrin.h>
#endif
#include <list>

extern "C" {
#include <libavformat/avformat.h>
}

#include "defs.h"
#include "shared/ffmpeg_raii.h"
#include "flags.h"
#include "shared/httpd.h"
#include "shared/memcpy_interleaved.h"
#include "pbo_frame_allocator.h"
#include "shared/timebase.h"
#include "va_display_with_cleanup.h"

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_x11.h>

using namespace bmusb;
using namespace std;

extern void memcpy_with_pitch(uint8_t *dst, const uint8_t *src, size_t src_width, size_t dst_pitch, size_t height);

#define CHECK_VASTATUS(va_status, func)                                 \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr, "%s:%d (%s) failed with %d\n", __func__, __LINE__, func, va_status); \
        exit(1);                                                        \
    }

// From libjpeg (although it's of course identical between implementations).
static const int jpeg_natural_order[DCTSIZE2] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

struct VectorDestinationManager {
	jpeg_destination_mgr pub;
	std::vector<uint8_t> dest;

	VectorDestinationManager()
	{
		pub.init_destination = init_destination_thunk;
		pub.empty_output_buffer = empty_output_buffer_thunk;
		pub.term_destination = term_destination_thunk;
	}

	static void init_destination_thunk(j_compress_ptr ptr)
	{
		((VectorDestinationManager *)(ptr->dest))->init_destination();
	}

	inline void init_destination()
	{
		make_room(0);
	}

	static boolean empty_output_buffer_thunk(j_compress_ptr ptr)
	{
		return ((VectorDestinationManager *)(ptr->dest))->empty_output_buffer();
	}

	inline bool empty_output_buffer()
	{
		make_room(dest.size());  // Should ignore pub.free_in_buffer!
		return true;
	}

	inline void make_room(size_t bytes_used)
	{
		dest.resize(bytes_used + 4096);
		dest.resize(dest.capacity());
		pub.next_output_byte = dest.data() + bytes_used;
		pub.free_in_buffer = dest.size() - bytes_used;
	}

	static void term_destination_thunk(j_compress_ptr ptr)
	{
		((VectorDestinationManager *)(ptr->dest))->term_destination();
	}

	inline void term_destination()
	{
		dest.resize(dest.size() - pub.free_in_buffer);
	}
};
static_assert(std::is_standard_layout<VectorDestinationManager>::value, "");

int MJPEGEncoder::write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	MJPEGEncoder *engine = (MJPEGEncoder *)opaque;
	return engine->write_packet2(buf, buf_size, type, time);
}

int MJPEGEncoder::write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	if (type == AVIO_DATA_MARKER_HEADER) {
		mux_header.append((char *)buf, buf_size);
		httpd->set_header(HTTPD::MULTICAM_STREAM, mux_header);
	} else {
		httpd->add_data(HTTPD::MULTICAM_STREAM, (char *)buf, buf_size, /*keyframe=*/true, AV_NOPTS_VALUE, AVRational{ AV_TIME_BASE, 1 });
	}
	return buf_size;
}

MJPEGEncoder::MJPEGEncoder(HTTPD *httpd, const string &va_display)
	: httpd(httpd)
{
	encoder_thread = thread(&MJPEGEncoder::encoder_thread_func, this);

	// Set up the mux. We don't use the Mux wrapper, because it's geared towards
	// a situation with only one video stream (and possibly one audio stream)
	// with known width/height, and we don't need the extra functionality it provides.
	avctx.reset(avformat_alloc_context());
	avctx->oformat = av_guess_format("mp4", nullptr, nullptr);

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &MJPEGEncoder::write_packet2_thunk;
	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	for (int card_idx = 0; card_idx < global_flags.num_cards; ++card_idx) {
		AVStream *stream = avformat_new_stream(avctx.get(), nullptr);
		if (stream == nullptr) {
			fprintf(stderr, "avformat_new_stream() failed\n");
			exit(1);
		}
		stream->time_base = AVRational{ 1, TIMEBASE };
		stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
		stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;

		// Used for aspect ratio only. Can change without notice (the mux won't care).
		stream->codecpar->width = global_flags.width;
		stream->codecpar->height = global_flags.height;

		// TODO: We could perhaps use the interpretation for each card here
		// (or at least the command-line flags) instead of the defaults,
		// but what would we do when they change?
		stream->codecpar->color_primaries = AVCOL_PRI_BT709;
		stream->codecpar->color_trc = AVCOL_TRC_IEC61966_2_1;
		stream->codecpar->color_space = AVCOL_SPC_BT709;
		stream->codecpar->color_range = AVCOL_RANGE_MPEG;
		stream->codecpar->chroma_location = AVCHROMA_LOC_LEFT;
		stream->codecpar->field_order = AV_FIELD_PROGRESSIVE;
	}

	AVDictionary *options = NULL;
	vector<pair<string, string>> opts = MUX_OPTS;
	for (pair<string, string> opt : opts) {
		av_dict_set(&options, opt.first.c_str(), opt.second.c_str(), 0);
	}
	if (avformat_write_header(avctx.get(), &options) < 0) {
		fprintf(stderr, "avformat_write_header() failed\n");
		exit(1);
	}

	// Initialize VA-API.
	string error;
	va_dpy = try_open_va(va_display, &error, &config_id);
	if (va_dpy == nullptr) {
		fprintf(stderr, "Could not initialize VA-API for MJPEG encoding: %s. JPEGs will be encoded in software if needed.\n", error.c_str());
	}

	running = true;
}

MJPEGEncoder::~MJPEGEncoder()
{
	av_free(avctx->pb->buffer);
}

void MJPEGEncoder::stop()
{
	if (!running) {
		return;
	}
	running = false;
	should_quit = true;
	any_frames_to_be_encoded.notify_all();
	encoder_thread.join();
}

unique_ptr<VADisplayWithCleanup> MJPEGEncoder::try_open_va(const string &va_display, string *error, VAConfigID *config_id)
{
	unique_ptr<VADisplayWithCleanup> va_dpy = va_open_display(va_display);
	if (va_dpy == nullptr) {
		if (error) *error = "Opening VA display failed";
		return nullptr;
	}
	int major_ver, minor_ver;
	VAStatus va_status = vaInitialize(va_dpy->va_dpy, &major_ver, &minor_ver);
	if (va_status != VA_STATUS_SUCCESS) {
		char buf[256];
		snprintf(buf, sizeof(buf), "vaInitialize() failed with status %d\n", va_status);
		if (error != nullptr) *error = buf;
		return nullptr;
	}

	VAConfigAttrib attr = { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV422 };
	va_status = vaCreateConfig(va_dpy->va_dpy, VAProfileJPEGBaseline, VAEntrypointEncPicture,
		&attr, 1, config_id);
	if (va_status == VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT) {
		if (error != nullptr) *error = "No hardware support";
		return nullptr;
	} else if (va_status != VA_STATUS_SUCCESS) {
		char buf[256];
		snprintf(buf, sizeof(buf), "vaCreateConfig() failed with status %d\n", va_status);
		if (error != nullptr) *error = buf;
		return nullptr;
	}

	int num_formats = vaMaxNumImageFormats(va_dpy->va_dpy);
	assert(num_formats > 0);

	unique_ptr<VAImageFormat[]> formats(new VAImageFormat[num_formats]);
	va_status = vaQueryImageFormats(va_dpy->va_dpy, formats.get(), &num_formats);
	if (va_status != VA_STATUS_SUCCESS) {
		char buf[256];
		snprintf(buf, sizeof(buf), "vaQueryImageFormats() failed with status %d\n", va_status);
		if (error != nullptr) *error = buf;
		return nullptr;
	}

	return va_dpy;
}

void MJPEGEncoder::upload_frame(int64_t pts, unsigned card_index, RefCountedFrame frame, const bmusb::VideoFormat &video_format, size_t y_offset, size_t cbcr_offset)
{
	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)frame->userdata;
	if (video_format.width == 0 || video_format.height == 0) {
		return;
	}
	if (video_format.interlaced) {
		fprintf(stderr, "Card %u: Ignoring JPEG encoding for interlaced frame\n", card_index);
		return;
	}
	if (userdata->pixel_format != PixelFormat_8BitYCbCr ||
	    !frame->interleaved) {
		fprintf(stderr, "Card %u: Ignoring JPEG encoding for unsupported pixel format\n", card_index);
		return;
	}
	if (video_format.width > 4096 || video_format.height > 4096) {
		fprintf(stderr, "Card %u: Ignoring JPEG encoding for oversized frame\n", card_index);
		return;
	}

	lock_guard<mutex> lock(mu);
	frames_to_be_encoded.push(QueuedFrame{ pts, card_index, frame, video_format, y_offset, cbcr_offset });
	any_frames_to_be_encoded.notify_all();
}

void MJPEGEncoder::encoder_thread_func()
{
	pthread_setname_np(pthread_self(), "MJPEG_Encode");
	posix_memalign((void **)&tmp_y, 4096, 4096 * 8);
	posix_memalign((void **)&tmp_cbcr, 4096, 4096 * 8);
	posix_memalign((void **)&tmp_cb, 4096, 4096 * 8);
	posix_memalign((void **)&tmp_cr, 4096, 4096 * 8);

	unique_lock<mutex> lock(mu);
	for (;;) {
		any_frames_to_be_encoded.wait(lock, [this] { return !frames_to_be_encoded.empty() || should_quit; });
		if (should_quit) break;
		QueuedFrame qf = move(frames_to_be_encoded.front());
		frames_to_be_encoded.pop();

		vector<uint8_t> jpeg = encode_jpeg(qf);

		AVPacket pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.buf = nullptr;
		pkt.data = &jpeg[0];
		pkt.size = jpeg.size();
		pkt.stream_index = qf.card_index;
		pkt.flags = AV_PKT_FLAG_KEY;
		pkt.pts = pkt.dts = qf.pts;

		if (av_write_frame(avctx.get(), &pkt) < 0) {
			fprintf(stderr, "av_write_frame() failed\n");
			exit(1);
		}
	}

	free(tmp_y);
	free(tmp_cbcr);
	free(tmp_cb);
	free(tmp_cr);
}

class VABufferDestroyer {
public:
	VABufferDestroyer(VADisplay dpy, VABufferID buf)
		: dpy(dpy), buf(buf) {}

	~VABufferDestroyer() {
		VAStatus va_status = vaDestroyBuffer(dpy, buf);
		CHECK_VASTATUS(va_status, "vaDestroyBuffer");
	}

private:
	VADisplay dpy;
	VABufferID buf;
};

MJPEGEncoder::VAResources MJPEGEncoder::get_va_resources(unsigned width, unsigned height)
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

	VASurfaceAttrib attrib;
	attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
	attrib.type = VASurfaceAttribPixelFormat;
	attrib.value.type = VAGenericValueTypeInteger;
	attrib.value.value.i = VA_FOURCC_UYVY;

	VAStatus va_status = vaCreateSurfaces(va_dpy->va_dpy, VA_RT_FORMAT_YUV422,
		width, height,
		&ret.surface, 1, &attrib, 1);
	CHECK_VASTATUS(va_status, "vaCreateSurfaces");

	va_status = vaCreateContext(va_dpy->va_dpy, config_id, width, height, 0, &ret.surface, 1, &ret.context);
	CHECK_VASTATUS(va_status, "vaCreateContext");

	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAEncCodedBufferType, width * height * 3 + 8192, 1, nullptr, &ret.data_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");

	return ret;
}

void MJPEGEncoder::release_va_resources(MJPEGEncoder::VAResources resources)
{
	lock_guard<mutex> lock(va_resources_mutex);
	if (va_resources_freelist.size() > 10) {
		auto it = va_resources_freelist.end();
		--it;

		VAStatus va_status = vaDestroyBuffer(va_dpy->va_dpy, it->data_buffer);
		CHECK_VASTATUS(va_status, "vaDestroyBuffer");

		va_status = vaDestroyContext(va_dpy->va_dpy, it->context);
		CHECK_VASTATUS(va_status, "vaDestroyContext");

		va_status = vaDestroySurfaces(va_dpy->va_dpy, &it->surface, 1);
		CHECK_VASTATUS(va_status, "vaDestroySurfaces");

		va_resources_freelist.erase(it);
	}

	va_resources_freelist.push_front(resources);
}

void MJPEGEncoder::init_jpeg_422(unsigned width, unsigned height, VectorDestinationManager *dest, jpeg_compress_struct *cinfo)
{
	jpeg_error_mgr jerr;
	cinfo->err = jpeg_std_error(&jerr);
	jpeg_create_compress(cinfo);

	cinfo->dest = (jpeg_destination_mgr *)dest;

	cinfo->input_components = 3;
	jpeg_set_defaults(cinfo);
	jpeg_set_quality(cinfo, quality, /*force_baseline=*/false);

	cinfo->image_width = width;
	cinfo->image_height = height;
	cinfo->raw_data_in = true;
	jpeg_set_colorspace(cinfo, JCS_YCbCr);
	cinfo->comp_info[0].h_samp_factor = 2;
	cinfo->comp_info[0].v_samp_factor = 1;
	cinfo->comp_info[1].h_samp_factor = 1;
	cinfo->comp_info[1].v_samp_factor = 1;
	cinfo->comp_info[2].h_samp_factor = 1;
	cinfo->comp_info[2].v_samp_factor = 1;
	cinfo->CCIR601_sampling = true;  // Seems to be mostly ignored by libjpeg, though.
	jpeg_start_compress(cinfo, true);

	// This comment marker is private to FFmpeg. It signals limited Y'CbCr range
	// (and nothing else).
	jpeg_write_marker(cinfo, JPEG_COM, (const JOCTET *)"CS=ITU601", strlen("CS=ITU601"));
}

vector<uint8_t> MJPEGEncoder::get_jpeg_header(unsigned width, unsigned height, jpeg_compress_struct *cinfo)
{
	VectorDestinationManager dest;
	init_jpeg_422(width, height, &dest, cinfo);

	// Make a dummy black image; there's seemingly no other easy way of
	// making libjpeg outputting all of its headers.
	JSAMPROW yptr[8], cbptr[8], crptr[8];
	JSAMPARRAY data[3] = { yptr, cbptr, crptr };
	memset(tmp_y, 0, 4096);
	memset(tmp_cb, 0, 4096);
	memset(tmp_cr, 0, 4096);
	for (unsigned yy = 0; yy < 8; ++yy) {
		yptr[yy] = tmp_y;
		cbptr[yy] = tmp_cb;
		crptr[yy] = tmp_cr;
	}
	for (unsigned y = 0; y < height; y += 8) {
		jpeg_write_raw_data(cinfo, data, /*num_lines=*/8);
	}
	jpeg_finish_compress(cinfo);

	// We're only interested in the header, not the data after it.
	dest.term_destination();
	for (size_t i = 0; i < dest.dest.size() - 1; ++i) {
		if (dest.dest[i] == 0xff && dest.dest[i + 1] == 0xda) {  // Start of scan (SOS).
			unsigned len = dest.dest[i + 2] * 256 + dest.dest[i + 3];
			dest.dest.resize(i + len + 2);
			break;
		}
	}

	return dest.dest;
}

MJPEGEncoder::VAData MJPEGEncoder::get_va_data_for_resolution(unsigned width, unsigned height)
{
	pair<unsigned, unsigned> key(width, height);
	if (va_data_for_resolution.count(key)) {
		return va_data_for_resolution[key];
	}

	// Use libjpeg to generate a header and set sane defaults for e.g.
	// quantization tables. Then do the actual encode with VA-API.
	jpeg_compress_struct cinfo;
	vector<uint8_t> jpeg_header = get_jpeg_header(width, height, &cinfo);

	// Picture parameters.
	VAEncPictureParameterBufferJPEG pic_param;
	memset(&pic_param, 0, sizeof(pic_param));
	pic_param.reconstructed_picture = VA_INVALID_ID;
	pic_param.picture_width = cinfo.image_width;
	pic_param.picture_height = cinfo.image_height;
	for (int component_idx = 0; component_idx < cinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &cinfo.comp_info[component_idx];
		pic_param.component_id[component_idx] = comp->component_id;
		pic_param.quantiser_table_selector[component_idx] = comp->quant_tbl_no;
	}
	pic_param.num_components = cinfo.num_components;
	pic_param.num_scan = 1;
	pic_param.sample_bit_depth = 8;
	pic_param.coded_buf = VA_INVALID_ID;  // To be filled out by caller.
	pic_param.pic_flags.bits.huffman = 1;
	pic_param.quality = 50;  // Don't scale the given quantization matrices. (See gen8_mfc_jpeg_fqm_state)

	// Quantization matrices.
	VAQMatrixBufferJPEG q;
	memset(&q, 0, sizeof(q));

	q.load_lum_quantiser_matrix = true;
	q.load_chroma_quantiser_matrix = true;
	for (int quant_tbl_idx = 0; quant_tbl_idx < min(4, NUM_QUANT_TBLS); ++quant_tbl_idx) {
		const JQUANT_TBL *qtbl = cinfo.quant_tbl_ptrs[quant_tbl_idx];
		assert((qtbl == nullptr) == (quant_tbl_idx >= 2));
		if (qtbl == nullptr) continue;

		uint8_t *qmatrix = (quant_tbl_idx == 0) ? q.lum_quantiser_matrix : q.chroma_quantiser_matrix;
		for (int i = 0; i < 64; ++i) {
			if (qtbl->quantval[i] > 255) {
				fprintf(stderr, "Baseline JPEG only!\n");
				abort();
			}
			qmatrix[i] = qtbl->quantval[jpeg_natural_order[i]];
		}
	}

	// Huffman tables (arithmetic is not supported).
	VAHuffmanTableBufferJPEGBaseline huff;
	memset(&huff, 0, sizeof(huff));

	for (int huff_tbl_idx = 0; huff_tbl_idx < min(2, NUM_HUFF_TBLS); ++huff_tbl_idx) {
		const JHUFF_TBL *ac_hufftbl = cinfo.ac_huff_tbl_ptrs[huff_tbl_idx];
		const JHUFF_TBL *dc_hufftbl = cinfo.dc_huff_tbl_ptrs[huff_tbl_idx];
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

	// Slice parameters (metadata about the slice).
	VAEncSliceParameterBufferJPEG parms;
	memset(&parms, 0, sizeof(parms));
	for (int component_idx = 0; component_idx < cinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &cinfo.comp_info[component_idx];
		parms.components[component_idx].component_selector = comp->component_id;
		parms.components[component_idx].dc_table_selector = comp->dc_tbl_no;
		parms.components[component_idx].ac_table_selector = comp->ac_tbl_no;
		if (parms.components[component_idx].dc_table_selector > 1 ||
		    parms.components[component_idx].ac_table_selector > 1) {
			fprintf(stderr, "Uses too many Huffman tables\n");
			abort();
		}
	}
	parms.num_components = cinfo.num_components;
	parms.restart_interval = cinfo.restart_interval;

	jpeg_destroy_compress(&cinfo);

	VAData ret;
	ret.jpeg_header = move(jpeg_header);
	ret.pic_param = pic_param;
	ret.q = q;
	ret.huff = huff;
	ret.parms = parms;
	va_data_for_resolution[key] = ret;
	return ret;
}

vector<uint8_t> MJPEGEncoder::encode_jpeg(const QueuedFrame &qf)
{
	if (va_dpy != nullptr) {
		return encode_jpeg_va(qf);
	} else {
		return encode_jpeg_libjpeg(qf);
	}
}

vector<uint8_t> MJPEGEncoder::encode_jpeg_va(const QueuedFrame &qf)
{
	unsigned width = qf.video_format.width;
	unsigned height = qf.video_format.height;

	VAResources resources = get_va_resources(width, height);
	ReleaseVAResources release(this, resources);

	VAData va_data = get_va_data_for_resolution(width, height);
	va_data.pic_param.coded_buf = resources.data_buffer;

	VABufferID pic_param_buffer;
	VAStatus va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAEncPictureParameterBufferType, sizeof(va_data.pic_param), 1, &va_data.pic_param, &pic_param_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_pic_param(va_dpy->va_dpy, pic_param_buffer);

	VABufferID q_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAQMatrixBufferType, sizeof(va_data.q), 1, &va_data.q, &q_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_iq(va_dpy->va_dpy, q_buffer);

	VABufferID huff_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAHuffmanTableBufferType, sizeof(va_data.huff), 1, &va_data.huff, &huff_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_huff(va_dpy->va_dpy, huff_buffer);

	VABufferID slice_param_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAEncSliceParameterBufferType, sizeof(va_data.parms), 1, &va_data.parms, &slice_param_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_slice_param(va_dpy->va_dpy, slice_param_buffer);

	VAImage image;
	va_status = vaDeriveImage(va_dpy->va_dpy, resources.surface, &image);
	CHECK_VASTATUS(va_status, "vaDeriveImage");

	// Upload the pixel data.
	uint8_t *surface_p = nullptr;
	vaMapBuffer(va_dpy->va_dpy, image.buf, (void **)&surface_p);

	size_t field_start_line = qf.video_format.extra_lines_top;  // No interlacing support.
	size_t field_start = qf.cbcr_offset * 2 + qf.video_format.width * field_start_line * 2;

	{
		const uint8_t *src = qf.frame->data_copy + field_start;
		uint8_t *dst = (unsigned char *)surface_p + image.offsets[0];
		memcpy_with_pitch(dst, src, qf.video_format.width * 2, image.pitches[0], qf.video_format.height);
	}

	va_status = vaUnmapBuffer(va_dpy->va_dpy, image.buf);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	va_status = vaDestroyImage(va_dpy->va_dpy, image.image_id);
	CHECK_VASTATUS(va_status, "vaDestroyImage");

	// Finally, stick in the JPEG header.
	VAEncPackedHeaderParameterBuffer header_parm;
	header_parm.type = VAEncPackedHeaderRawData;
	header_parm.bit_length = 8 * va_data.jpeg_header.size();

	VABufferID header_parm_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAEncPackedHeaderParameterBufferType, sizeof(header_parm), 1, &header_parm, &header_parm_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_header(va_dpy->va_dpy, header_parm_buffer);

	VABufferID header_data_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, config_id, VAEncPackedHeaderDataBufferType, va_data.jpeg_header.size(), 1, va_data.jpeg_header.data(), &header_data_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_header_data(va_dpy->va_dpy, header_data_buffer);

	va_status = vaBeginPicture(va_dpy->va_dpy, resources.context, resources.surface);
	CHECK_VASTATUS(va_status, "vaBeginPicture");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &pic_param_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(pic_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &q_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(q)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &huff_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(huff)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &slice_param_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(slice_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &header_parm_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(header_parm)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &header_data_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(header_data)");
	va_status = vaEndPicture(va_dpy->va_dpy, resources.context);
	CHECK_VASTATUS(va_status, "vaEndPicture");

	va_status = vaSyncSurface(va_dpy->va_dpy, resources.surface);
	CHECK_VASTATUS(va_status, "vaSyncSurface");

	VACodedBufferSegment *segment;
	va_status = vaMapBuffer(va_dpy->va_dpy, resources.data_buffer, (void **)&segment);
	CHECK_VASTATUS(va_status, "vaMapBuffer");

	const char *coded_buf = reinterpret_cast<char *>(segment->buf);
	vector<uint8_t> jpeg(coded_buf, coded_buf + segment->size); 

	va_status = vaUnmapBuffer(va_dpy->va_dpy, resources.data_buffer);
	CHECK_VASTATUS(va_status, "vaUnmapBuffer");

	return jpeg;
}

vector<uint8_t> MJPEGEncoder::encode_jpeg_libjpeg(const QueuedFrame &qf)
{
	unsigned width = qf.video_format.width;
	unsigned height = qf.video_format.height;

	VectorDestinationManager dest;
	jpeg_compress_struct cinfo;
	init_jpeg_422(width, height, &dest, &cinfo);

	size_t field_start_line = qf.video_format.extra_lines_top;  // No interlacing support.
	size_t field_start = qf.cbcr_offset * 2 + qf.video_format.width * field_start_line * 2;

	JSAMPROW yptr[8], cbptr[8], crptr[8];
	JSAMPARRAY data[3] = { yptr, cbptr, crptr };
	for (unsigned y = 0; y < qf.video_format.height; y += 8) {
	        const uint8_t *src = qf.frame->data_copy + field_start + y * qf.video_format.width * 2;

	        memcpy_interleaved(tmp_y, tmp_cbcr, src, qf.video_format.width * 8 * 2);
	        memcpy_interleaved(tmp_cb, tmp_cr, tmp_cbcr, qf.video_format.width * 8);
	        for (unsigned yy = 0; yy < 8; ++yy) {
	                yptr[yy] = tmp_y + yy * width;
	                cbptr[yy] = tmp_cb + yy * width / 2;
	                crptr[yy] = tmp_cr + yy * width / 2;
		}
	        jpeg_write_raw_data(&cinfo, data, /*num_lines=*/8);
	}
	jpeg_finish_compress(&cinfo);

	return dest.dest;
}
