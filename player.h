#ifndef _PLAYER_H
#define _PLAYER_H 1

#include "clip_list.h"

extern "C" {
#include <libavformat/avio.h>
}

#include <condition_variable>
#include <functional>
#include <mutex>

class JPEGFrameView;
class VideoStream;
class QSurface;
class QSurfaceFormat;

class Player {
public:
	Player(JPEGFrameView *destination, bool also_output_to_stream);

	void play_clip(const Clip &clip, unsigned stream_idx);
	void override_angle(unsigned stream_idx);  // For the current clip only.

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	using done_callback_func = std::function<void()>;
	void set_done_callback(done_callback_func cb) { done_callback = cb; }

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	using next_clip_callback_func = std::function<Clip()>;
	void set_next_clip_callback(next_clip_callback_func cb) { next_clip_callback = cb; }

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	using progress_callback_func = std::function<void(double played_this_clip, double total_length)>;
	void set_progress_callback(progress_callback_func cb) { progress_callback = cb; }

private:
	void thread_func(bool also_output_to_stream);
	void open_output_stream();
	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	// Find the frame immediately before and after this point.
	// Returns false if pts is after the last frame.
	bool find_surrounding_frames(int64_t pts, int stream_idx, int64_t *pts_lower, int64_t *pts_upper);

	JPEGFrameView *destination;
	done_callback_func done_callback;
	next_clip_callback_func next_clip_callback;
	progress_callback_func progress_callback;

	std::mutex mu;
	Clip current_clip;  // Under mu. Can have pts_in = -1 for no clip.
	unsigned current_stream_idx;  // Under mu.

	std::mutex queue_state_mu;
	std::condition_variable new_clip_changed;
	bool new_clip_ready = false;  // Under queue_state_mu.
	bool playing = false;  // Under queue_state_mu.
	int override_stream_idx = -1;  // Under queue_state_mu.

	std::unique_ptr<VideoStream> video_stream;  // Can be nullptr.
};

#endif  // !defined(_PLAYER_H)
