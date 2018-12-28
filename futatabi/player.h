#ifndef _PLAYER_H
#define _PLAYER_H 1

#include "clip_list.h"
#include "frame_on_disk.h"
#include "queue_spot_holder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class JPEGFrameView;
class VideoStream;
class QSurface;
class QSurfaceFormat;

class Player : public QueueInterface {
public:
	enum StreamOutput {
		NO_STREAM_OUTPUT,
		HTTPD_STREAM_OUTPUT,  // Output to global_httpd.
		FILE_STREAM_OUTPUT    // Output to file_avctx.
	};
	Player(JPEGFrameView *destination, StreamOutput stream_output, AVFormatContext *file_avctx = nullptr);
	~Player();

	void play(const Clip &clip)
	{
		play({ ClipWithID{ clip, 0 } });
	}
	void play(const std::vector<ClipWithID> &clips);
	void override_angle(unsigned stream_idx);  // Assumes one-clip playlist only.

	// Replace the part of the playlist that we haven't started playing yet
	// (ie., from the point immediately after the last current playing clip
	// and to the end) with the given one.
	//
	// E.g., if we have the playlist A, B, C, D, E, F, we're currently in a fade
	// from B to C and run splice_play() with the list G, C, H, I, the resulting
	// list will be A, B, C, H, I. (If the new list doesn't contain B nor C,
	// there will be some heuristics.) Note that we always compare on ID only;
	// changes will be ignored for the purposes of setting the split point,
	// although the newly-spliced entries will of course get the new in/out points
	// etc., which is the main reason for going through this exercise in the first
	// place.
	//
	// If nothing is playing, the call will be ignored.
	void splice_play(const std::vector<ClipWithID> &clips);

	// Set the status string that will be used for the video stream's status subtitles
	// whenever we are not playing anything.
	void set_pause_status(const std::string &status)
	{
		std::lock_guard<std::mutex> lock(queue_state_mu);
		pause_status = status;
	}

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	using done_callback_func = std::function<void()>;
	void set_done_callback(done_callback_func cb) { done_callback = cb; }

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	// The keys in the given map are row members in the vector given to play().
	using progress_callback_func = std::function<void(const std::map<uint64_t, double> &progress, double time_remaining)>;
	void set_progress_callback(progress_callback_func cb) { progress_callback = cb; }

	// QueueInterface.
	void take_queue_spot() override;
	void release_queue_spot() override;

private:
	void thread_func(AVFormatContext *file_avctx);
	void play_playlist_once();
	void display_single_frame(int primary_stream_idx, const FrameOnDisk &primary_frame, int secondary_stream_idx, const FrameOnDisk &secondary_frame, double fade_alpha, std::chrono::steady_clock::time_point frame_start, bool snapped, const std::string &subtitle);
	void open_output_stream();
	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	// Find the frame immediately before and after this point.
	// Returns false if pts is after the last frame.
	bool find_surrounding_frames(int64_t pts, int stream_idx, FrameOnDisk *frame_lower, FrameOnDisk *frame_upper);

	std::thread player_thread;
	std::atomic<bool> should_quit{ false };

	JPEGFrameView *destination;
	done_callback_func done_callback;
	progress_callback_func progress_callback;

	std::mutex queue_state_mu;
	std::condition_variable new_clip_changed;
	std::vector<ClipWithID> queued_clip_list;  // Under queue_state_mu.
	bool new_clip_ready = false;  // Under queue_state_mu.
	bool playing = false;  // Under queue_state_mu.
	int override_stream_idx = -1;  // Under queue_state_mu.
	int64_t last_pts_played = -1;  // Under queue_state_mu. Used by previews only.

	bool splice_ready = false;  // Under queue_state_mu.
	std::vector<ClipWithID> to_splice_clip_list;  // Under queue_state_mu.
	std::string pause_status = "paused";  // Under queue_state_mu.

	std::unique_ptr<VideoStream> video_stream;  // Can be nullptr.

	std::atomic<int64_t> metric_dropped_interpolated_frame{ 0 };
	std::atomic<int64_t> metric_dropped_unconditional_frame{ 0 };
	std::atomic<int64_t> metric_faded_frame{ 0 };
	std::atomic<int64_t> metric_faded_snapped_frame{ 0 };
	std::atomic<int64_t> metric_original_frame{ 0 };
	std::atomic<int64_t> metric_original_snapped_frame{ 0 };
	std::atomic<int64_t> metric_refresh_frame{ 0 };
	std::atomic<int64_t> metric_interpolated_frame{ 0 };
	std::atomic<int64_t> metric_interpolated_faded_frame{ 0 };

	// under queue_state_mu. Part of this instead of VideoStream so that we own
	// its lock and can sleep on it.
	size_t num_queued_frames = 0;
	static constexpr size_t max_queued_frames = 10;

	// State private to the player thread.
	int64_t pts = 0;
	const StreamOutput stream_output;
};

double compute_time_left(const std::vector<ClipWithID> &clips, size_t currently_playing_idx, double progress_currently_playing);

static inline double compute_total_time(const std::vector<ClipWithID> &clips)
{
	return compute_time_left(clips, 0, 0.0);
}

std::string format_duration(double t);

#endif  // !defined(_PLAYER_H)
