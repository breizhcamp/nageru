#ifndef _PLAYER_H
#define _PLAYER_H 1

#include "clip_list.h"

#include <condition_variable>
#include <mutex>

class JPEGFrameView;

class Player {
public:
	Player(JPEGFrameView *destination);

	void play_clip(const Clip &clip, unsigned stream_idx);

private:
	void thread_func();

	JPEGFrameView *destination;

	std::mutex mu;
	Clip current_clip;  // Under mu.
	unsigned current_stream_idx;  // Under mu.

	enum { PAUSED, PLAYING } cue_state = PAUSED;  // Under cue_state_mu.
	std::mutex cue_state_mu;
	std::condition_variable cue_is_playing;
};

#endif  // !defined(_PLAYER_H)
