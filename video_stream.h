#ifndef _VIDEO_STREAM_H
#define _VIDEO_STREAM_H 1

#include <epoxy/gl.h>
#include <stdint.h>

extern "C" {
#include <libavformat/avio.h>
}

#include "jpeg_frame_view.h"
#include "ref_counted_gl_sync.h"

#include <condition_variable>
#include <deque>
#include <movit/effect_chain.h>
#include <movit/mix_effect.h>
#include <movit/ycbcr_input.h>
#include <mutex>
#include <string>
#include <thread>

class ChromaSubsampler;
class DISComputeFlow;
class Interpolate;
class Mux;
class QSurface;
class QSurfaceFormat;
class YCbCrConverter;

class VideoStream {
public:
	VideoStream();
	~VideoStream();
	void start();
	void stop();

	void schedule_original_frame(int64_t output_pts, unsigned stream_idx, int64_t input_pts);
	void schedule_faded_frame(int64_t output_pts, unsigned stream_idx, int64_t input_pts, int secondary_stream_idx, int64_t secondary_input_pts, float fade_alpha);
	void schedule_interpolated_frame(int64_t output_pts, unsigned stream_idx, int64_t input_first_pts, int64_t input_second_pts, float alpha, int secondary_stream_idx = -1, int64_t secondary_inputs_pts = -1, float fade_alpha = 0.0f);  // -1 = no secondary frame.
	void schedule_refresh_frame(int64_t output_pts);

private:
	void encode_thread_func();
	std::thread encode_thread;

	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	// Allocated at the very start; if we're empty, we start dropping frames
	// (so that we don't build up an infinite interpolation backlog).
	struct InterpolatedFrameResources {
		GLuint input_tex;  // Layered (contains both input frames), Y'CbCr.
		GLuint gray_tex;  // Same, but Y only.
		GLuint input_fbos[2];  // For rendering to the two layers of input_tex.

		// Destination textures and FBO if there is a fade.
		GLuint fade_y_output_tex, fade_cbcr_output_tex;
		GLuint fade_fbo;

		GLuint cb_tex, cr_tex;  // Subsampled, final output.

		GLuint pbo;  // For reading the data back.
		void *pbo_contents;  // Persistently mapped.
	};
	std::deque<InterpolatedFrameResources> interpolate_resources;  // Under <queue_lock>.
	static constexpr size_t num_interpolate_slots = 10;

	struct QueuedFrame {
		int64_t output_pts;
		enum Type { ORIGINAL, FADED, INTERPOLATED, FADED_INTERPOLATED } type;
		unsigned stream_idx;
		int64_t input_first_pts;  // The only pts for original frames.

		// For fades only (including fades against interpolated frames).
		int secondary_stream_idx = -1;
		int64_t secondary_input_pts;

		// For interpolated frames only.
		int64_t input_second_pts;
		float alpha;
		InterpolatedFrameResources resources;
		RefCountedGLsync fence;  // Set when the interpolated image is read back to the CPU.
		GLuint flow_tex, output_tex, cbcr_tex;  // Released in the receiving thread; not really used for anything else.
		JPEGID id;
	};
	std::deque<QueuedFrame> frame_queue;  // Under <queue_lock>.
	std::mutex queue_lock;
	std::condition_variable queue_nonempty;

	std::unique_ptr<Mux> stream_mux;  // To HTTP.
	std::string stream_mux_header;
	bool seen_sync_markers = false;

	std::unique_ptr<YCbCrConverter> ycbcr_converter;
	std::unique_ptr<YCbCrConverter> ycbcr_semiplanar_converter;

	// Frame interpolation.
	std::unique_ptr<DISComputeFlow> compute_flow;
	std::unique_ptr<Interpolate> interpolate, interpolate_no_split;
	std::unique_ptr<ChromaSubsampler> chroma_subsampler;

	std::vector<uint8_t> last_frame;
};

#endif  // !defined(_VIDEO_STREAM_H)
