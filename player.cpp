#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "clip_list.h"
#include "defs.h"
#include "jpeg_frame_view.h"
#include "player.h"

using namespace std;
using namespace std::chrono;

extern mutex frame_mu;
extern vector<int64_t> frames[MAX_STREAMS];

void Player::thread_func()
{
	for ( ;; ) {
		// Wait until we're supposed to play something.
		{
			unique_lock<mutex> lock(cue_state_mu);
			cue_is_playing.wait(lock, [this]{
				return cue_state == PLAYING;
				//return current_cue_status.origin != steady_clock::time_point::max();
			});
		}

		Clip clip;
		unsigned stream_idx;
		{
			lock_guard<mutex> lock2(mu);
			clip = current_clip;
			stream_idx = current_stream_idx;
		}
		steady_clock::time_point origin = steady_clock::now();
		int64_t pts_origin = clip.pts_in;

		int64_t next_pts = pts_origin;

		bool eof = false;
		while (!eof) {  // TODO: check for abort
			// FIXME: assumes a given timebase.
			double speed = 0.5;
			steady_clock::time_point next_frame_start =
				origin + microseconds((next_pts - pts_origin) * int(1000000 / speed) / 12800);
			this_thread::sleep_until(next_frame_start);
			destination->setFrame(stream_idx, next_pts);

			// Find the next frame.
			{
				lock_guard<mutex> lock2(frame_mu);
				auto it = upper_bound(frames[stream_idx].begin(),
					frames[stream_idx].end(),
					next_pts);
				if (it == frames[stream_idx].end()) {
					eof = true;
				} else {
					next_pts = *it;
					if (next_pts >= clip.pts_out) {
						eof = true;
					}
				}
			}
			if (eof) break;
		}

		// TODO: callback so that the next playlist item can be cued.
		{
			unique_lock<mutex> lock(cue_state_mu);
			cue_state = PAUSED;
		}
	}
}

Player::Player(JPEGFrameView *destination)
	: destination(destination)
{
	thread(&Player::thread_func, this).detach();
}

void Player::play_clip(const Clip &clip, unsigned stream_idx)
{
	{
		lock_guard<mutex> lock(mu);
		current_clip = clip;
		current_stream_idx = stream_idx;
	}

	{
		lock_guard<mutex> lock(cue_state_mu);
		cue_state = PLAYING;
		cue_is_playing.notify_all();
	}
}
