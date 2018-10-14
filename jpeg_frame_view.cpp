#include "jpeg_frame_view.h"

#include "defs.h"
#include "post_to_main_thread.h"
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

// Just an arbitrary order for std::map.
struct JPEGIDLexicalOrder
{
	bool operator() (const JPEGID &a, const JPEGID &b) const
	{
		if (a.stream_idx != b.stream_idx)
			return a.stream_idx < b.stream_idx;
		if (a.pts != b.pts)
			return a.pts < b.pts;
		return a.interpolated < b.interpolated;
	}
};

struct LRUFrame {
	shared_ptr<Frame> frame;
	size_t last_used;
};

struct PendingDecode {
	JPEGID primary, secondary;
	float fade_alpha;  // Irrelevant if secondary.stream_idx == -1.
	JPEGFrameView *destination;
};

thread JPEGFrameView::jpeg_decoder_thread;
mutex cache_mu;
map<JPEGID, LRUFrame, JPEGIDLexicalOrder> cache;  // Under cache_mu.
condition_variable any_pending_decodes, cache_updated;
deque<PendingDecode> pending_decodes;  // Under cache_mu.
atomic<size_t> event_counter{0};
extern QGLWidget *global_share_widget;
extern atomic<bool> should_quit;

shared_ptr<Frame> decode_jpeg(const string &filename)
{
	shared_ptr<Frame> frame;
	if (vaapi_jpeg_decoding_usable) {
		frame = decode_jpeg_vaapi(filename);
		if (frame != nullptr) {
			return frame;
		}
		fprintf(stderr, "VA-API hardware decoding failed; falling back to software.\n");
	}

	frame.reset(new Frame);

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

shared_ptr<Frame> decode_jpeg_with_cache(JPEGID id, CacheMissBehavior cache_miss_behavior, bool *did_decode)
{
	*did_decode = false;
	{
		unique_lock<mutex> lock(cache_mu);
		auto it = cache.find(id);
		if (it != cache.end()) {
			it->second.last_used = event_counter++;
			return it->second.frame;
		}
	}

	if (cache_miss_behavior == RETURN_NULLPTR_IF_NOT_IN_CACHE) {
		return nullptr;
	}

	assert(!id.interpolated);
	*did_decode = true;
	shared_ptr<Frame> frame = decode_jpeg(filename_for_frame(id.stream_idx, id.pts));

	unique_lock<mutex> lock(cache_mu);
	cache[id] = LRUFrame{ frame, event_counter++ };

	if (cache.size() > CACHE_SIZE) {
		prune_cache();
	}
	return frame;
}

void jpeg_decoder_thread_func()
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

		shared_ptr<Frame> primary_frame, secondary_frame;
		bool drop = false;
		for (int subframe_idx = 0; subframe_idx < 2; ++subframe_idx) {
			const JPEGID &id = (subframe_idx == 0 ? decode.primary : decode.secondary);
			if (id.stream_idx == (unsigned)-1) {
				// No secondary frame.
				continue;
			}

			bool found_in_cache;
			shared_ptr<Frame> frame;
			if (id.interpolated) {
				// Interpolated frames are never decoded by us,
				// put directly into the cache from VideoStream.
				unique_lock<mutex> lock(cache_mu);
				cache_updated.wait(lock, [id] {
					return cache.count(id) != 0 || should_quit.load();
				});
				if (should_quit.load())
					break;
				found_in_cache = true;  // Don't count it as a decode.

				auto it = cache.find(id);
				assert(it != cache.end());

				it->second.last_used = event_counter++;
				frame = it->second.frame;
				if (frame == nullptr) {
					// We inserted a nullptr as signal that the frame was never
					// interpolated and that we should stop waiting.
					// But don't let it linger in the cache anymore.
					cache.erase(it);
				}
			} else {
				frame = decode_jpeg_with_cache(id, cache_miss_behavior, &found_in_cache);
			}

			if (frame == nullptr) {
				assert(id.interpolated || cache_miss_behavior == RETURN_NULLPTR_IF_NOT_IN_CACHE);
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
				primary_frame = move(frame);
			} else {
				secondary_frame = move(frame);
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
}

void JPEGFrameView::setFrame(unsigned stream_idx, int64_t pts, bool interpolated, int secondary_stream_idx, int64_t secondary_pts, float fade_alpha)
{
	current_stream_idx = stream_idx;  // TODO: Does this interact with fades?

	unique_lock<mutex> lock(cache_mu);
	PendingDecode decode;
	if (interpolated && secondary_stream_idx != -1) {
		// The frame will already be faded for us, so ask for only one; we shouldn't fade it against anything.
		decode.primary = create_jpegid_for_interpolated_fade(stream_idx, pts, secondary_stream_idx, secondary_pts);
		decode.secondary = JPEGID{ (unsigned)-1, -1, /*interpolated=*/false };
	} else {
		decode.primary = JPEGID{ stream_idx, pts, interpolated };
		decode.secondary = JPEGID{ (unsigned)secondary_stream_idx, secondary_pts, /*interpolated=*/false };
	}
	decode.fade_alpha = fade_alpha;
	decode.destination = this;
	pending_decodes.push_back(decode);
	any_pending_decodes.notify_all();
}

void JPEGFrameView::insert_interpolated_frame(JPEGID id, shared_ptr<Frame> frame)
{
	// We rely on the frame not being evicted from the cache before
	// jpeg_decoder_thread() sees it and can display it (otherwise,
	// that thread would hang). With a default cache of 1000 elements,
	// that would sound like a reasonable assumption.
	unique_lock<mutex> lock(cache_mu);
	cache[id] = LRUFrame{ std::move(frame), event_counter++ };
	cache_updated.notify_all();
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
