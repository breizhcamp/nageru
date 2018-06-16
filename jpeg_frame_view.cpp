#include "jpeg_frame_view.h"

#include <jpeglib.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <movit/resource_pool.h>
#include <movit/init.h>
#include <movit/util.h>

#include "defs.h"
#include "post_to_main_thread.h"

using namespace movit;
using namespace std;

string filename_for_frame(unsigned stream_idx, int64_t pts);

struct JPEGID {
	unsigned stream_idx;
	int64_t pts;
};
bool operator< (const JPEGID &a, const JPEGID &b) {
	return make_pair(a.stream_idx, a.pts) < make_pair(b.stream_idx, b.pts);
}

struct LRUFrame {
	shared_ptr<Frame> frame;
	size_t last_used;
};

mutex cache_mu;
map<JPEGID, LRUFrame> cache;  // Under cache_mu.
condition_variable any_pending_decodes;
deque<pair<JPEGID, JPEGFrameView *>> pending_decodes;  // Under cache_mu.
atomic<size_t> event_counter{0};
extern QGLWidget *global_share_widget;

// TODO: Decode using VA-API if available.
shared_ptr<Frame> decode_jpeg(const string &filename)
{
	shared_ptr<Frame> frame(new Frame);

	jpeg_decompress_struct dinfo;
	jpeg_error_mgr jerr;
	dinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&dinfo);

	FILE *fp = fopen(filename.c_str(), "rb");
	if (fp == nullptr) {
		perror(filename.c_str());
		exit(1);
	}
	jpeg_stdio_src(&dinfo, fp);

	jpeg_read_header(&dinfo, true);

	if (dinfo.num_components != 3) {
		fprintf(stderr, "Not a color JPEG. (%d components, Y=%dx%d, Cb=%dx%d, Cr=%dx%d)\n",
			dinfo.num_components,
			dinfo.comp_info[0].h_samp_factor, dinfo.comp_info[0].v_samp_factor,
			dinfo.comp_info[1].h_samp_factor, dinfo.comp_info[1].v_samp_factor,
			dinfo.comp_info[2].h_samp_factor, dinfo.comp_info[2].v_samp_factor);
		exit(1);
	}
	if (dinfo.comp_info[0].h_samp_factor != dinfo.max_h_samp_factor ||
	    dinfo.comp_info[0].v_samp_factor != dinfo.max_v_samp_factor ||  // Y' must not be subsampled.
	    dinfo.comp_info[1].h_samp_factor != dinfo.comp_info[2].h_samp_factor ||
	    dinfo.comp_info[1].v_samp_factor != dinfo.comp_info[2].v_samp_factor ||  // Cb and Cr must be identically subsampled.
	    (dinfo.max_h_samp_factor % dinfo.comp_info[1].h_samp_factor) != 0 ||
	    (dinfo.max_v_samp_factor % dinfo.comp_info[1].v_samp_factor) != 0) {  // No 2:3 subsampling or other weirdness.
		fprintf(stderr, "Unsupported subsampling scheme. (Y=%dx%d, Cb=%dx%d, Cr=%dx%d)\n",
			dinfo.comp_info[0].h_samp_factor, dinfo.comp_info[0].v_samp_factor,
			dinfo.comp_info[1].h_samp_factor, dinfo.comp_info[1].v_samp_factor,
			dinfo.comp_info[2].h_samp_factor, dinfo.comp_info[2].v_samp_factor);
		exit(1);
	}
	dinfo.raw_data_out = true;

	jpeg_start_decompress(&dinfo);

	frame->width = dinfo.output_width;
	frame->height = dinfo.output_height;
	frame->chroma_subsampling_x = dinfo.max_h_samp_factor / dinfo.comp_info[1].h_samp_factor;
	frame->chroma_subsampling_y = dinfo.max_v_samp_factor / dinfo.comp_info[1].v_samp_factor;

	unsigned h_mcu_size = DCTSIZE * dinfo.max_h_samp_factor;
	unsigned v_mcu_size = DCTSIZE * dinfo.max_v_samp_factor;
	unsigned chroma_width_blocks = (dinfo.output_width + h_mcu_size - 1) / h_mcu_size;
	unsigned width_blocks = chroma_width_blocks * dinfo.max_h_samp_factor;
	unsigned chroma_height_blocks = (dinfo.output_height + v_mcu_size - 1) / v_mcu_size;
	unsigned height_blocks = chroma_height_blocks * dinfo.max_v_samp_factor;

	// TODO: Decode into a PBO.
	frame->y.reset(new uint8_t[width_blocks * height_blocks * DCTSIZE2]);
	frame->cb.reset(new uint8_t[chroma_width_blocks * chroma_height_blocks * DCTSIZE2]);
	frame->cr.reset(new uint8_t[chroma_width_blocks * chroma_height_blocks * DCTSIZE2]);

	JSAMPROW yptr[v_mcu_size], cbptr[v_mcu_size], crptr[v_mcu_size];
	JSAMPARRAY data[3] = { yptr, cbptr, crptr };
	for (unsigned y = 0; y < chroma_height_blocks; ++y) {
		// NOTE: The last elements of cbptr/crptr will be unused for vertically subsampled chroma.
		for (unsigned yy = 0; yy < v_mcu_size; ++yy) {
			yptr[yy] = frame->y.get() + (y * DCTSIZE * dinfo.max_v_samp_factor + yy) * width_blocks * DCTSIZE;
			cbptr[yy] = frame->cb.get() + (y * DCTSIZE * dinfo.comp_info[1].v_samp_factor + yy) * chroma_width_blocks * DCTSIZE;
			crptr[yy] = frame->cr.get() + (y * DCTSIZE * dinfo.comp_info[1].v_samp_factor + yy) * chroma_width_blocks * DCTSIZE;
		}

		jpeg_read_raw_data(&dinfo, data, v_mcu_size);
	}

	(void) jpeg_finish_decompress(&dinfo);
	jpeg_destroy_decompress(&dinfo);
	fclose(fp);

	return frame;
}

void prune_cache()
{
	// Assumes cache_mu is held.
	vector<size_t> lru_timestamps;
	for (const auto &key_and_value : cache) {
		lru_timestamps.push_back(key_and_value.second.last_used);
	}

	size_t cutoff_point = CACHE_SIZE / 10;  // Prune away the 10% oldest ones.
	nth_element(lru_timestamps.begin(), lru_timestamps.begin() + cutoff_point, lru_timestamps.end());
	size_t must_be_used_after = lru_timestamps[cutoff_point];
	for (auto it = cache.begin(); it != cache.end(); ) {
		if (it->second.last_used < must_be_used_after) {
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

void jpeg_decoder_thread()
{
	size_t num_decoded = 0, num_dropped = 0;

	pthread_setname_np(pthread_self(), "JPEGDecoder");
	for ( ;; ) {
		JPEGID id;
		JPEGFrameView *dest;
		shared_ptr<Frame> frame;
		{
			unique_lock<mutex> lock(cache_mu);
			any_pending_decodes.wait(lock, [] {
				return !pending_decodes.empty();
			});
			id = pending_decodes.front().first;
			dest = pending_decodes.front().second;
			pending_decodes.pop_front();

			auto it = cache.find(id);
			if (it != cache.end()) {
				frame = it->second.frame;
				it->second.last_used = event_counter++;
			}
		}

		if (frame == nullptr) {
			// Not found in the cache, so we need to do a decode or drop the request.
			// Prune the queue if there are too many pending for this destination.
			// TODO: Could we get starvation here?
			size_t num_pending = 0;
			for (const pair<JPEGID, JPEGFrameView *> &decode : pending_decodes) {
				if (decode.second == dest) {
					++num_pending;
				}
			}
			if (num_pending > 3) {
				++num_dropped;
				continue;
			}

			frame = decode_jpeg(filename_for_frame(id.stream_idx, id.pts));

			unique_lock<mutex> lock(cache_mu);
			cache[id] = LRUFrame{ frame, event_counter++ };

			if (cache.size() > CACHE_SIZE) {
				prune_cache();
			}
			++num_decoded;
			if (num_decoded % 1000 == 0) {
				fprintf(stderr, "Decoded %zu images, dropped %zu (%.2f%% dropped)\n",
					num_decoded, num_dropped, (100.0 * num_dropped) / (num_decoded + num_dropped));
			}
		}

		dest->setDecodedFrame(frame);
	}
}

JPEGFrameView::JPEGFrameView(QWidget *parent)
	: QGLWidget(parent, global_share_widget) {
}

void JPEGFrameView::update_frame()
{
	unique_lock<mutex> lock(cache_mu);
	pending_decodes.emplace_back(JPEGID{ stream_idx, pts }, this);
	any_pending_decodes.notify_all();
}

ResourcePool *resource_pool = nullptr;

void JPEGFrameView::initializeGL()
{
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	check_error();

	static once_flag once;
	call_once(once, [] {
		CHECK(init_movit(MOVIT_SHADER_DIR, MOVIT_DEBUG_OFF));
		resource_pool = new ResourcePool;

		std::thread(&jpeg_decoder_thread).detach();
	});

	chain.reset(new EffectChain(1280, 720, resource_pool));
	ImageFormat image_format;
	image_format.color_space = COLORSPACE_sRGB;
	image_format.gamma_curve = GAMMA_sRGB;
	ycbcr_format.luma_coefficients = YCBCR_REC_709;
	ycbcr_format.full_range = false;
	ycbcr_format.num_levels = 256;
	ycbcr_format.chroma_subsampling_x = 2;
	ycbcr_format.chroma_subsampling_y = 1;
	ycbcr_format.cb_x_position = 0.0f;  // H.264 -- _not_ JPEG, even though our input is MJPEG-encoded
	ycbcr_format.cb_y_position = 0.5f;  // Irrelevant.
	ycbcr_format.cr_x_position = 0.0f;
	ycbcr_format.cr_y_position = 0.5f;
	ycbcr_input = (movit::YCbCrInput *)chain->add_input(new YCbCrInput(image_format, ycbcr_format, 1280, 720));

	ImageFormat inout_format;
        inout_format.color_space = COLORSPACE_sRGB;
        inout_format.gamma_curve = GAMMA_sRGB;

	check_error();
	chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
	check_error();
	chain->set_dither_bits(8);
	check_error();
	chain->finalize();
	check_error();
}

void JPEGFrameView::resizeGL(int width, int height)
{
	check_error();
	glViewport(0, 0, width, height);
	check_error();
}

void JPEGFrameView::paintGL()
{
	//glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
	//glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	check_error();
	chain->render_to_screen();
}

void JPEGFrameView::setDecodedFrame(std::shared_ptr<Frame> frame)
{
	post_to_main_thread([this, frame] {
		current_frame = frame;
		int width_blocks = (frame->width + 15) / 16;
		ycbcr_input->set_width(frame->width);
		ycbcr_input->set_height(frame->height);
		ycbcr_input->set_pixel_data(0, frame->y.get());
		ycbcr_input->set_pixel_data(1, frame->cb.get());
		ycbcr_input->set_pixel_data(2, frame->cr.get());
		ycbcr_input->set_pitch(0, width_blocks * 16);
		ycbcr_input->set_pitch(1, width_blocks * 8);
		ycbcr_input->set_pitch(2, width_blocks * 8);
		ycbcr_format.chroma_subsampling_x = frame->chroma_subsampling_x;
		ycbcr_format.chroma_subsampling_y = frame->chroma_subsampling_y;
		ycbcr_input->change_ycbcr_format(ycbcr_format);
		update();
	});
}
