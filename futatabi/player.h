#ifndef _PLAYER_H
#define _PLAYER_H 1

#include "clip_list.h"
#include "frame_on_disk.h"
#include "queue_spot_holder.h"

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

class Player : public QueueInterface {
public:
	Player(JPEGFrameView *destination, bool also_output_to_stream);

	void play_clip(const Clip &clip, size_t clip_idx, unsigned stream_idx);
	void override_angle(unsigned stream_idx);  // For the current clip only.

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	using done_callback_func = std::function<void()>;
	void set_done_callback(done_callback_func cb) { done_callback = cb; }

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	// The second parameter is the clip's position in the play list.
	using next_clip_callback_func = std::function<std::pair<Clip, size_t>()>;
	void set_next_clip_callback(next_clip_callback_func cb) { next_clip_callback = cb; }

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	using progress_callback_func = std::function<void(const std::map<size_t, double> &progress)>;
	void set_progress_callback(progress_callback_func cb) { progress_callback = cb; }

	// QueueInterface.
	void take_queue_spot() override;
	void release_queue_spot() override;

private:
	void thread_func(bool also_output_to_stream);
	void open_output_stream();
	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	// Find the frame immediately before and after this point.
	// Returns false if pts is after the last frame.
	bool find_surrounding_frames(int64_t pts, int stream_idx, FrameOnDisk *frame_lower, FrameOnDisk *frame_upper);

	JPEGFrameView *destination;
	done_callback_func done_callback;
	next_clip_callback_func next_clip_callback;
	progress_callback_func progress_callback;

	std::mutex mu;
	Clip current_clip;  // Under mu. Can have pts_in = -1 for no clip.
	size_t current_clip_idx;  // Under mu.
	unsigned current_stream_idx;  // Under mu.

	std::mutex queue_state_mu;
	std::condition_variable new_clip_changed;
	bool new_clip_ready = false;  // Under queue_state_mu.
	bool playing = false;  // Under queue_state_mu.
	int override_stream_idx = -1;  // Under queue_state_mu.

	std::unique_ptr<VideoStream> video_stream;  // Can be nullptr.

	// under queue_state_mu. Part of this instead of VideoStream so that we own
	// its lock and can sleep on it.
	size_t num_queued_frames = 0;
	static constexpr size_t max_queued_frames = 10;
};

#endif  // !defined(_PLAYER_H)