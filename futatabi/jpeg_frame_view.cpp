#include "jpeg_frame_view.h"

#include "defs.h"
#include "flags.h"
#include "jpeg_destroyer.h"
#include "jpeglib_error_wrapper.h"
#include "shared/metrics.h"
#include "shared/post_to_main_thread.h"
#include "video_stream.h"
#include "ycbcr_converter.h"

#include <QMouseEvent>
#include <QScreen>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <jpeglib.h>
#include <movit/init.h>
#include <movit/resource_pool.h>
#include <movit/util.h>
#include <mutex>
#include <stdint.h>
#include <thread>
#include <unistd.h>
#include <utility>

// Must come after the Qt stuff.
#include "vaapi_jpeg_decoder.h"

using namespace movit;
using namespace std;

namespace {

// Just an arbitrary order for std::map.
struct FrameOnDiskLexicalOrder
{
	bool operator() (const FrameOnDisk &a, const FrameOnDisk &b) const
	{
		if (a.pts != b.pts)
			return a.pts < b.pts;
		if (a.offset != b.offset)
			return a.offset < b.offset;
		if (a.filename_idx != b.filename_idx)
			return a.filename_idx < b.filename_idx;
		assert(a.size == b.size);
		return false;
	}
};

inline size_t frame_size(const Frame &frame)
{
	size_t y_size = frame.width * frame.height;
	size_t cbcr_size = y_size / frame.chroma_subsampling_x / frame.chroma_subsampling_y;
	return y_size + cbcr_size * 2;
}

struct LRUFrame {
	shared_ptr<Frame> frame;
	size_t last_used;
};

struct PendingDecode {
	JPEGFrameView *destination;

	// For actual decodes (only if frame below is nullptr).
	FrameOnDisk primary, secondary;
	float fade_alpha;  // Irrelevant if secondary.stream_idx == -1.

	// Already-decoded frames are also sent through PendingDecode,
	// so that they get drawn in the right order. If frame is nullptr,
	// it's a real decode.
	shared_ptr<Frame> frame;
};

// There can be multiple JPEGFrameView instances, so make all the metrics static.
once_flag jpeg_metrics_inited;
atomic<int64_t> metric_jpeg_cache_used_bytes{0};  // Same value as cache_bytes_used.
atomic<int64_t> metric_jpeg_cache_limit_bytes{size_t(CACHE_SIZE_MB) * 1024 * 1024};
atomic<int64_t> metric_jpeg_cache_given_up_frames{0};
atomic<int64_t> metric_jpeg_cache_hit_frames{0};
atomic<int64_t> metric_jpeg_cache_miss_frames{0};
atomic<int64_t> metric_jpeg_software_decode_frames{0};
atomic<int64_t> metric_jpeg_software_fail_frames{0};
atomic<int64_t> metric_jpeg_vaapi_decode_frames{0};
atomic<int64_t> metric_jpeg_vaapi_fail_frames{0};

}  // namespace

thread JPEGFrameView::jpeg_decoder_thread;
mutex cache_mu;
map<FrameOnDisk, LRUFrame, FrameOnDiskLexicalOrder> cache;  // Under cache_mu.
size_t cache_bytes_used = 0;  // Under cache_mu.
condition_variable any_pending_decodes;
deque<PendingDecode> pending_decodes;  // Under cache_mu.
atomic<size_t> event_counter{0};
extern QGLWidget *global_share_widget;
extern atomic<bool> should_quit;

shared_ptr<Frame> decode_jpeg(const string &jpeg)
{
	shared_ptr<Frame> frame;
	if (vaapi_jpeg_decoding_usable) {
		frame = decode_jpeg_vaapi(jpeg);
		if (frame != nullptr) {
			++metric_jpeg_vaapi_decode_frames;
			return frame;
		}
		fprintf(stderr, "VA-API hardware decoding failed; falling back to software.\n");
		++metric_jpeg_vaapi_fail_frames;
	}

	frame.reset(new Frame);

	jpeg_decompress_struct dinfo;
	JPEGWrapErrorManager error_mgr(&dinfo);
	if (!error_mgr.run([&dinfo]{ jpeg_create_decompress(&dinfo); })) {
		return get_black_frame();
	}
	JPEGDestroyer destroy_dinfo(&dinfo);

	if (!error_mgr.run([&dinfo, &jpeg]{
		jpeg_mem_src(&dinfo, reinterpret_cast<const unsigned char *>(jpeg.data()), jpeg.size());
		jpeg_read_header(&dinfo, true);
	})) {
		return get_black_frame();
	}

	if (dinfo.num_components != 3) {
		fprintf(stderr, "Not a color JPEG. (%d components, Y=%dx%d, Cb=%dx%d, Cr=%dx%d)\n",
			dinfo.num_components,
			dinfo.comp_info[0].h_samp_factor, dinfo.comp_info[0].v_samp_factor,
			dinfo.comp_info[1].h_samp_factor, dinfo.comp_info[1].v_samp_factor,
			dinfo.comp_info[2].h_samp_factor, dinfo.comp_info[2].v_samp_factor);
		return get_black_frame();
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

	if (!error_mgr.run([&dinfo]{
		jpeg_start_decompress(&dinfo);
	})) {
		return get_black_frame();
	}

	frame->width = dinfo.output_width;
	frame->height = dinfo.output_height;
	frame->chroma_subsampling_x = dinfo.max_h_samp_factor / dinfo.comp_info[1].h_samp_factor;
	frame->chroma_subsampling_y = dinfo.max_v_samp_factor / dinfo.comp_info[1].v_samp_factor;

	unsigned h_mcu_size = DCTSIZE * dinfo.max_h_samp_factor;
	unsigned v_mcu_size = DCTSIZE * dinfo.max_v_samp_factor;
	unsigned mcu_width_blocks = (dinfo.output_width + h_mcu_size - 1) / h_mcu_size;
	unsigned mcu_height_blocks = (dinfo.output_height + v_mcu_size - 1) / v_mcu_size;

	unsigned luma_width_blocks = mcu_width_blocks * dinfo.comp_info[0].h_samp_factor;
	unsigned chroma_width_blocks = mcu_width_blocks * dinfo.comp_info[1].h_samp_factor;
	unsigned luma_height_blocks = mcu_height_blocks * dinfo.comp_info[0].v_samp_factor;
	unsigned chroma_height_blocks = mcu_height_blocks * dinfo.comp_info[1].v_samp_factor;

	// TODO: Decode into a PBO.
	frame->y.reset(new uint8_t[luma_width_blocks * luma_height_blocks * DCTSIZE2]);
	frame->cb.reset(new uint8_t[chroma_width_blocks * chroma_height_blocks * DCTSIZE2]);
	frame->cr.reset(new uint8_t[chroma_width_blocks * chroma_height_blocks * DCTSIZE2]);
	frame->pitch_y = luma_width_blocks * DCTSIZE;
	frame->pitch_chroma = chroma_width_blocks * DCTSIZE;

	if (!error_mgr.run([&dinfo, &frame, v_mcu_size, mcu_height_blocks] {
		JSAMPROW yptr[v_mcu_size], cbptr[v_mcu_size], crptr[v_mcu_size];
		JSAMPARRAY data[3] = { yptr, cbptr, crptr };
		for (unsigned y = 0; y < mcu_height_blocks; ++y) {
			// NOTE: The last elements of cbptr/crptr will be unused for vertically subsampled chroma.
			for (unsigned yy = 0; yy < v_mcu_size; ++yy) {
				yptr[yy] = frame->y.get() + (y * DCTSIZE * dinfo.max_v_samp_factor + yy) * frame->pitch_y;
				cbptr[yy] = frame->cb.get() + (y * DCTSIZE * dinfo.comp_info[1].v_samp_factor + yy) * frame->pitch_chroma;
				crptr[yy] = frame->cr.get() + (y * DCTSIZE * dinfo.comp_info[1].v_samp_factor + yy) * frame->pitch_chroma;
			}

			jpeg_read_raw_data(&dinfo, data, v_mcu_size);
		}

		(void)jpeg_finish_decompress(&dinfo);
	})) {
		return get_black_frame();
	}

	++metric_jpeg_software_decode_frames;
	return frame;
}

void prune_cache()
{
	// Assumes cache_mu is held.
	int64_t bytes_still_to_remove = cache_bytes_used - (size_t(CACHE_SIZE_MB) * 1024 * 1024) * 9 / 10;
	if (bytes_still_to_remove <= 0) return;

	vector<pair<size_t, size_t>> lru_timestamps_and_size;
	for (const auto &key_and_value : cache) {
		lru_timestamps_and_size.emplace_back(
			key_and_value.second.last_used,
			frame_size(*key_and_value.second.frame));
	}
	sort(lru_timestamps_and_size.begin(), lru_timestamps_and_size.end());

	// Remove the oldest ones until we are below 90% of the cache used.
	size_t lru_cutoff_point = 0;
	for (const pair<size_t, size_t> &it : lru_timestamps_and_size) {
		lru_cutoff_point = it.first;
		bytes_still_to_remove -= it.second;
		if (bytes_still_to_remove <= 0) break;
	}

	for (auto it = cache.begin(); it != cache.end(); ) {
		if (it->second.last_used <= lru_cutoff_point) {
			cache_bytes_used -= frame_size(*it->second.frame);
			metric_jpeg_cache_used_bytes = cache_bytes_used;
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

shared_ptr<Frame> decode_jpeg_with_cache(FrameOnDisk frame_spec, CacheMissBehavior cache_miss_behavior, FrameReader *frame_reader, bool *did_decode)
{
	*did_decode = false;
	{
		lock_guard<mutex> lock(cache_mu);
		auto it = cache.find(frame_spec);
		if (it != cache.end()) {
			++metric_jpeg_cache_hit_frames;
			it->second.last_used = event_counter++;
			return it->second.frame;
		}
	}

	if (cache_miss_behavior == RETURN_NULLPTR_IF_NOT_IN_CACHE) {
		++metric_jpeg_cache_given_up_frames;
		return nullptr;
	}

	++metric_jpeg_cache_miss_frames;

	*did_decode = true;
	shared_ptr<Frame> frame = decode_jpeg(frame_reader->read_frame(frame_spec));

	lock_guard<mutex> lock(cache_mu);
	cache_bytes_used += frame_size(*frame);
	metric_jpeg_cache_used_bytes = cache_bytes_used;
	cache[frame_spec] = LRUFrame{ frame, event_counter++ };

	if (cache_bytes_used > size_t(CACHE_SIZE_MB) * 1024 * 1024) {
		prune_cache();
	}
	return frame;
}

void JPEGFrameView::jpeg_decoder_thread_func()
{
	size_t num_decoded = 0, num_dropped = 0;

	pthread_setname_np(pthread_self(), "JPEGDecoder");
	while (!should_quit.load()) {
		PendingDecode decode;
		CacheMissBehavior cache_miss_behavior = DECODE_IF_NOT_IN_CACHE;
		{
			unique_lock<mutex> lock(cache_mu);  // TODO: Perhaps under another lock?
			any_pending_decodes.wait(lock, [] {
				return !pending_decodes.empty() || should_quit.load();
			});
			if (should_quit.load())
				break;
			decode = pending_decodes.front();
			pending_decodes.pop_front();

			size_t num_pending = 0;
			for (const PendingDecode &other_decode : pending_decodes) {
				if (other_decode.destination == decode.destination) {
					++num_pending;
				}
			}
			if (num_pending > 3) {
				cache_miss_behavior = RETURN_NULLPTR_IF_NOT_IN_CACHE;
			}
		}

		if (decode.frame != nullptr) {
			// Already decoded, so just show it.
			decode.destination->setDecodedFrame(decode.frame, nullptr, 1.0f);
			continue;
		}

		shared_ptr<Frame> primary_frame, secondary_frame;
		bool drop = false;
		for (int subframe_idx = 0; subframe_idx < 2; ++subframe_idx) {
			const FrameOnDisk &frame_spec = (subframe_idx == 0 ? decode.primary : decode.secondary);
			if (frame_spec.pts == -1) {
				// No secondary frame.
				continue;
			}

			bool found_in_cache;
			shared_ptr<Frame> frame = decode_jpeg_with_cache(frame_spec, cache_miss_behavior, &decode.destination->frame_reader, &found_in_cache);

			if (frame == nullptr) {
				assert(cache_miss_behavior == RETURN_NULLPTR_IF_NOT_IN_CACHE);
				drop = true;
				break;
			}

			if (!found_in_cache) {
				++num_decoded;
				if (num_decoded % 1000 == 0) {
					fprintf(stderr, "Decoded %zu images, dropped %zu (%.2f%% dropped)\n",
						num_decoded, num_dropped, (100.0 * num_dropped) / (num_decoded + num_dropped));
				}
			}
			if (subframe_idx == 0) {
				primary_frame = std::move(frame);
			} else {
				secondary_frame = std::move(frame);
			}
		}
		if (drop) {
			++num_dropped;
			continue;
		}

		// TODO: Could we get jitter between non-interpolated and interpolated frames here?
		decode.destination->setDecodedFrame(primary_frame, secondary_frame, decode.fade_alpha);
	}
}

void JPEGFrameView::shutdown()
{
	any_pending_decodes.notify_all();
	jpeg_decoder_thread.join();
}

JPEGFrameView::JPEGFrameView(QWidget *parent)
	: QGLWidget(parent, global_share_widget)
{
	call_once(jpeg_metrics_inited, []{
		global_metrics.add("jpeg_cache_used_bytes", &metric_jpeg_cache_used_bytes, Metrics::TYPE_GAUGE);
		global_metrics.add("jpeg_cache_limit_bytes", &metric_jpeg_cache_limit_bytes, Metrics::TYPE_GAUGE);
		global_metrics.add("jpeg_cache_frames", {{ "action", "given_up" }}, &metric_jpeg_cache_given_up_frames);
		global_metrics.add("jpeg_cache_frames", {{ "action", "hit" }}, &metric_jpeg_cache_hit_frames);
		global_metrics.add("jpeg_cache_frames", {{ "action", "miss" }}, &metric_jpeg_cache_miss_frames);
		global_metrics.add("jpeg_decode_frames", {{ "decoder", "software" }, { "result", "decode" }}, &metric_jpeg_software_decode_frames);
		global_metrics.add("jpeg_decode_frames", {{ "decoder", "software" }, { "result", "fail" }}, &metric_jpeg_software_fail_frames);
		global_metrics.add("jpeg_decode_frames", {{ "decoder", "vaapi" }, { "result", "decode" }}, &metric_jpeg_vaapi_decode_frames);
		global_metrics.add("jpeg_decode_frames", {{ "decoder", "vaapi" }, { "result", "fail" }}, &metric_jpeg_vaapi_fail_frames);
	});
}

void JPEGFrameView::setFrame(unsigned stream_idx, FrameOnDisk frame, FrameOnDisk secondary_frame, float fade_alpha)
{
	current_stream_idx = stream_idx;  // TODO: Does this interact with fades?

	lock_guard<mutex> lock(cache_mu);
	PendingDecode decode;
	decode.primary = frame;
	decode.secondary = secondary_frame;
	decode.fade_alpha = fade_alpha;
	decode.destination = this;
	pending_decodes.push_back(decode);
	any_pending_decodes.notify_all();
}

void JPEGFrameView::setFrame(shared_ptr<Frame> frame)
{
	lock_guard<mutex> lock(cache_mu);
	PendingDecode decode;
	decode.frame = std::move(frame);
	decode.destination = this;
	pending_decodes.push_back(decode);
	any_pending_decodes.notify_all();
}

ResourcePool *resource_pool = nullptr;

void JPEGFrameView::initializeGL()
{
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	check_error();

	static once_flag once;
	call_once(once, [] {
		resource_pool = new ResourcePool;
		jpeg_decoder_thread = std::thread(jpeg_decoder_thread_func);
	});

	ycbcr_converter.reset(new YCbCrConverter(YCbCrConverter::OUTPUT_TO_RGBA, resource_pool));

	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	overlay_chain.reset(new EffectChain(overlay_base_width, overlay_base_height, resource_pool));
	overlay_input = (movit::FlatInput *)overlay_chain->add_input(new FlatInput(inout_format, FORMAT_GRAYSCALE, GL_UNSIGNED_BYTE, overlay_base_width, overlay_base_height));

	overlay_chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
	overlay_chain->finalize();
}

void JPEGFrameView::resizeGL(int width, int height)
{
	check_error();
	glViewport(0, 0, width, height);
	check_error();

	// Save these, as width() and height() will lie with DPI scaling.
	gl_width = width;
	gl_height = height;
}

void JPEGFrameView::paintGL()
{
	glViewport(0, 0, gl_width, gl_height);
	if (current_frame == nullptr) {
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	check_error();
	current_chain->render_to_screen();

	if (overlay_image != nullptr) {
		if (overlay_input_needs_refresh) {
			overlay_input->set_width(overlay_width);
			overlay_input->set_height(overlay_height);
			overlay_input->set_pixel_data(overlay_image->bits());
		}
		glViewport(gl_width - overlay_width, 0, overlay_width, overlay_height);
		overlay_chain->render_to_screen();
	}
}

namespace {

}  // namespace

void JPEGFrameView::setDecodedFrame(shared_ptr<Frame> frame, shared_ptr<Frame> secondary_frame, float fade_alpha)
{
	post_to_main_thread([this, frame, secondary_frame, fade_alpha] {
		current_frame = frame;
		current_secondary_frame = secondary_frame;

		if (secondary_frame != nullptr) {
			current_chain = ycbcr_converter->prepare_chain_for_fade(frame, secondary_frame, fade_alpha);
		} else {
			current_chain = ycbcr_converter->prepare_chain_for_conversion(frame);
		}
		update();
	});
}

void JPEGFrameView::mousePressEvent(QMouseEvent *event)
{
	if (event->type() == QEvent::MouseButtonPress && event->button() == Qt::LeftButton) {
		emit clicked();
	}
}

void JPEGFrameView::set_overlay(const string &text)
{
	if (text.empty()) {
		overlay_image.reset();
		return;
	}

	float dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
	overlay_width = lrint(overlay_base_width * dpr);
	overlay_height = lrint(overlay_base_height * dpr);

	overlay_image.reset(new QImage(overlay_width, overlay_height, QImage::Format_Grayscale8));
	overlay_image->setDevicePixelRatio(dpr);
	overlay_image->fill(0);
	QPainter painter(overlay_image.get());

	painter.setPen(Qt::white);
	QFont font = painter.font();
	font.setPointSize(12);
	painter.setFont(font);

	painter.drawText(QRectF(0, 0, overlay_base_width, overlay_base_height), Qt::AlignCenter, QString::fromStdString(text));

	// Don't refresh immediately; we might not have an OpenGL context here.
	overlay_input_needs_refresh = true;
}

shared_ptr<Frame> get_black_frame()
{
	static shared_ptr<Frame> black_frame;
	static once_flag flag;
	call_once(flag, [] {
		black_frame.reset(new Frame);
		black_frame->y.reset(new uint8_t[global_flags.width * global_flags.height]);
		black_frame->cb.reset(new uint8_t[(global_flags.width / 2) * (global_flags.height / 2)]);
		black_frame->cr.reset(new uint8_t[(global_flags.width / 2) * (global_flags.height / 2)]);
		black_frame->width = global_flags.width;
		black_frame->height = global_flags.height;
		black_frame->chroma_subsampling_x = 2;
		black_frame->chroma_subsampling_y = 2;
		black_frame->pitch_y = global_flags.width;
		black_frame->pitch_chroma = global_flags.width / 2;
	});
	++metric_jpeg_software_fail_frames;
	return black_frame;
}
