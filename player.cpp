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
			playing = true;
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

		int64_t next_pts = pts_origin - 1;  // Make sure we play the frame at clip.pts_in if it exists.

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
				new_clip_changed.wait_until(lock, next_frame_start, [this]{
					return new_clip_ready || override_stream_idx != -1;
				});
				if (new_clip_ready) break;
				if (override_stream_idx != -1) {
					stream_idx = override_stream_idx;
					override_stream_idx = -1;
					continue;
				}
			}

			destination->setFrame(stream_idx, next_pts);

		}

		{
			unique_lock<mutex> lock(queue_state_mu);
			playing = false;
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
		override_stream_idx = -1;
		new_clip_changed.notify_all();
	}
}

void Player::override_angle(unsigned stream_idx)
{
	// Corner case: If a new clip is waiting to be played, change its stream and then we're done. 
	{
		unique_lock<mutex> lock(queue_state_mu);
		if (new_clip_ready) {
			lock_guard<mutex> lock2(mu);
			current_stream_idx = stream_idx;
			return;
		}
	}

	// If we are playing a clip, set override_stream_idx, and the player thread will
	// pick it up and change its internal index.
	{
		unique_lock<mutex> lock(queue_state_mu);
		if (playing) {
			override_stream_idx = stream_idx;
			new_clip_changed.notify_all();
		}
	}

	// OK, so we're standing still, presumably at the end of a clip.
	// Look at the current pts_out (if it exists), and show the closest
	// thing we've got.
	int64_t pts_out;
	{
		lock_guard<mutex> lock(mu);
		if (current_clip.pts_out < 0) {
			return;
		}
		pts_out = current_clip.pts_out;
	}
			
	lock_guard<mutex> lock(frame_mu);
	auto it = upper_bound(frames[stream_idx].begin(), frames[stream_idx].end(), pts_out);
	if (it == frames[stream_idx].end()) {
		return;
	}
	destination->setFrame(stream_idx, *it);
}
