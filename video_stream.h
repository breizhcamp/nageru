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

class Mux;

class VideoStream {
public:
	void start();
	void stop();

	void schedule_original_frame(int64_t output_pts, unsigned stream_idx, int64_t input_pts);
	void schedule_interpolated_frame(int64_t output_pts, unsigned stream_idx, int64_t input_first_pts, int64_t input_second_pts, float alpha);

private:
	void encode_thread_func();
	std::thread encode_thread;

	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	struct QueuedFrame {
		int64_t output_pts;
		enum Type { ORIGINAL, INTERPOLATED } type;
		unsigned stream_idx;
		int64_t input_first_pts;  // The only pts for original frames.	

		// For interpolated frames only.
		int64_t input_second_pts;
		float alpha;
		GLuint flow_tex;
		GLuint fence;  // Set when the flow is done computing.
	};
	std::deque<QueuedFrame> frame_queue;  // Under <queue_lock>.
	std::mutex queue_lock;
	std::condition_variable queue_nonempty;

	std::unique_ptr<Mux> stream_mux;  // To HTTP.
	std::string stream_mux_header;
	bool seen_sync_markers = false;
};

#endif  // !defined(_VIDEO_STREAM_H)
