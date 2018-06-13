#ifndef _PLAYER_H
#define _PLAYER_H 1

#include "clip_list.h"

#include <condition_variable>
#include <functional>
#include <mutex>

class JPEGFrameView;

class Player {
public:
	Player(JPEGFrameView *destination);

	void play_clip(const Clip &clip, unsigned stream_idx);

	// Not thread-safe to set concurrently with playing.
	// Will be called back from the player thread.
	using done_callback_func = std::function<void()>;
	void set_done_callback(done_callback_func cb) { done_callback = cb; }

private:
	void thread_func();

	JPEGFrameView *destination;
	done_callback_func done_callback;

	std::mutex mu;
	Clip current_clip;  // Under mu.
	unsigned current_stream_idx;  // Under mu.

	enum { PAUSED, PLAYING } cue_state = PAUSED;  // Under cue_state_mu.
	std::mutex cue_state_mu;
	std::condition_variable cue_is_playing;
};

#endif  // !defined(_PLAYER_H)
