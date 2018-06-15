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
			unique_lock<mutex> lock(queue_state_mu);
			new_clip_changed.wait(lock, [this]{
				return new_clip_ready && current_clip.pts_in != -1;
			});
			new_clip_ready = false;
		}

		Clip clip;
		unsigned stream_idx;
		{
			lock_guard<mutex> lock(mu);
			clip = current_clip;
			stream_idx = current_stream_idx;
		}
		steady_clock::time_point origin = steady_clock::now();
		int64_t pts_origin = clip.pts_in;

		int64_t next_pts = pts_origin;

		bool aborted = false;
		for ( ;; ) {
			// Find the next frame.
			{
				lock_guard<mutex> lock(frame_mu);
				auto it = upper_bound(frames[stream_idx].begin(),
					frames[stream_idx].end(),
					next_pts);
				if (it == frames[stream_idx].end() || *it >= clip.pts_out) {
					break;
				}
				next_pts = *it;
			}

			// FIXME: assumes a given timebase.
			double speed = 0.5;
			steady_clock::time_point next_frame_start =
				origin + microseconds((next_pts - pts_origin) * int(1000000 / speed) / 12800);

			// Sleep until the next frame start, or until there's a new clip we're supposed to play.
			{
				unique_lock<mutex> lock(queue_state_mu);
				aborted = new_clip_changed.wait_until(lock, next_frame_start, [this]{
					return new_clip_ready;
				});
				if (aborted) break;
			}

			destination->setFrame(stream_idx, next_pts);

		}

		if (done_callback != nullptr && !aborted) {
			done_callback();
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
		lock_guard<mutex> lock(queue_state_mu);
		new_clip_ready = true;
		new_clip_changed.notify_all();
	}
}
