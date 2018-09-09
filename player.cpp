#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <stdio.h>

#include <movit/util.h>

#include "clip_list.h"
#include "context.h"
#include "defs.h"
#include "ffmpeg_raii.h"
#include "httpd.h"
#include "jpeg_frame_view.h"
#include "mux.h"
#include "player.h"
#include "video_stream.h"

using namespace std;
using namespace std::chrono;

extern mutex frame_mu;
extern vector<int64_t> frames[MAX_STREAMS];
extern HTTPD *global_httpd;

void Player::thread_func(bool also_output_to_stream)
{
	QSurface *surface = create_surface();
	QOpenGLContext *context = create_context(surface);
	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}

	check_error();

	// Create the VideoStream object, now that we have an OpenGL context.
	if (also_output_to_stream) {
		video_stream.reset(new VideoStream);
		video_stream->start();
	}
	
	check_error();

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

			if (video_stream != nullptr) {
				// Send the frame to the stream.
				// FIXME: Vaguely less crazy pts, perhaps.
				double pts_float = fmod(duration<double>(next_frame_start.time_since_epoch()).count(), 86400.0f);
				int64_t pts = lrint(pts_float * TIMEBASE);
				video_stream->schedule_original_frame(pts, stream_idx, next_pts);

				// HACK: test interpolation by frame-doubling.
				int64_t next_next_pts = -1;
				{
					lock_guard<mutex> lock(frame_mu);
					auto it = upper_bound(frames[stream_idx].begin(),
						frames[stream_idx].end(),
						next_pts);
					if (it == frames[stream_idx].end() || *it >= clip.pts_out) {
						break;
					}
					next_next_pts = *it;
				}
				if (next_next_pts != -1) {
					auto frame_len = microseconds((next_next_pts - next_pts) * int(1000000 / speed) / 12800) / 2;
					int64_t interpolated_pts = pts + lrint(duration<double>(frame_len).count() * TIMEBASE);
					video_stream->schedule_interpolated_frame(interpolated_pts, stream_idx, next_pts, next_next_pts, 0.5f);
				}
			}
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

Player::Player(JPEGFrameView *destination, bool also_output_to_stream)
	: destination(destination)
{
	thread(&Player::thread_func, this, also_output_to_stream).detach();
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
