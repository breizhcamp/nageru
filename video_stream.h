#ifndef _VIDEO_STREAM_H
#define _VIDEO_STREAM_H 1

#include <stdint.h>
#include <epoxy/gl.h>

extern "C" {
#include <libavformat/avio.h>
}

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <movit/effect_chain.h>
#include <movit/ycbcr_input.h>

#include "ref_counted_gl_sync.h"

class ChromaSubsampler;
class DISComputeFlow;
class Interpolate;
class Mux;
class QSurface;
class QSurfaceFormat;

class VideoStream {
public:
	VideoStream();
	~VideoStream();
	void start();
	void stop();

	void schedule_original_frame(int64_t output_pts, unsigned stream_idx, int64_t input_pts);
	void schedule_interpolated_frame(int64_t output_pts, unsigned stream_idx, int64_t input_first_pts, int64_t input_second_pts, float alpha);

private:

	void encode_thread_func();
	std::thread encode_thread;

	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	// Allocated at the very start; if we're empty, we start dropping frames
	// (so that we don't build up an infinite interpolation backlog).
	struct InterpolatedFrameResources {
		GLuint input_tex;  // Layered (contains both input frames).
		GLuint gray_tex;  // Same.
		GLuint cb_tex, cr_tex;
		GLuint input_fbos[2];  // For rendering to the two layers of input_tex.
		GLuint pbo;  // For reading the data back.
		void *pbo_contents;  // Persistently mapped.
	};
	std::deque<InterpolatedFrameResources> interpolate_resources;  // Under <queue_lock>.
	static constexpr size_t num_interpolate_slots = 10;

	struct QueuedFrame {
		int64_t output_pts;
		enum Type { ORIGINAL, INTERPOLATED } type;
		unsigned stream_idx;
		int64_t input_first_pts;  // The only pts for original frames.	

		// For interpolated frames only.
		int64_t input_second_pts;
		float alpha;
		InterpolatedFrameResources resources;
		RefCountedGLsync fence;  // Set when the interpolated image is read back to the CPU.
		GLuint flow_tex, output_tex, cbcr_tex;  // Released in the receiving thread; not really used for anything else.
	};
	std::deque<QueuedFrame> frame_queue;  // Under <queue_lock>.
	std::mutex queue_lock;
	std::condition_variable queue_nonempty;

	std::unique_ptr<Mux> stream_mux;  // To HTTP.
	std::string stream_mux_header;
	bool seen_sync_markers = false;

	// Effectively only converts from 4:2:2 to 4:4:4.
	// TODO: Have a separate version with ResampleEffect, for scaling?
	std::unique_ptr<movit::EffectChain> ycbcr_planar_convert_chain;
	std::unique_ptr<movit::EffectChain> ycbcr_semiplanar_convert_chain;

	movit::YCbCrInput *ycbcr_planar_input;
	movit::YCbCrInput *ycbcr_semiplanar_input;
	movit::YCbCrFormat ycbcr_format;

	// Frame interpolation.
	std::unique_ptr<DISComputeFlow> compute_flow;
	std::unique_ptr<Interpolate> interpolate;
	std::unique_ptr<ChromaSubsampler> chroma_subsampler;
};

#endif  // !defined(_VIDEO_STREAM_H)
