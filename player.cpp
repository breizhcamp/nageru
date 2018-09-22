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
#include "timebase.h"
#include "video_stream.h"

using namespace std;
using namespace std::chrono;

extern mutex frame_mu;
extern vector<int64_t> frames[MAX_STREAMS];
extern HTTPD *global_httpd;

void Player::thread_func(bool also_output_to_stream)
{
	pthread_setname_np(pthread_self(), "Player");

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

	constexpr double output_framerate = 60000.0 / 1001.0;  // FIXME: make configurable
	int64_t pts = 0;

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
		int64_t in_pts_origin = clip.pts_in;
		int64_t out_pts_origin = pts;

		// Start playing exactly at a frame.
		{
			lock_guard<mutex> lock(frame_mu);

			// Find the first frame such that frame.pts <= in_pts.
			auto it = lower_bound(frames[stream_idx].begin(),
				frames[stream_idx].end(),
				in_pts_origin);
			if (it != frames[stream_idx].end()) {
				in_pts_origin = *it;
			}
		}

		// TODO: Lock to a rational multiple of the frame rate if possible.
		double speed = 0.5;

		bool aborted = false;
		for (int frameno = 0; ; ++frameno) {  // Ends when the clip ends.
			double out_pts = out_pts_origin + TIMEBASE * frameno / output_framerate;
			steady_clock::time_point next_frame_start =
				origin + microseconds(lrint((out_pts - out_pts_origin) * 1e6 / TIMEBASE));
			int64_t in_pts = lrint(in_pts_origin + TIMEBASE * frameno * speed / output_framerate);
			pts = lrint(out_pts);

			int64_t in_pts_lower, in_pts_upper;

			// Find the frame immediately before and after this point.
			{
				lock_guard<mutex> lock(frame_mu);

				// Find the first frame such that in_pts >= frame.pts.
				auto it = lower_bound(frames[stream_idx].begin(),
					frames[stream_idx].end(),
					in_pts);
				if (it == frames[stream_idx].end() || *it >= clip.pts_out) {
					break;
				}
				in_pts_upper = *it;

				// Find the last frame such that in_pts <= frame.pts (if any).
				if (it == frames[stream_idx].begin()) {
					in_pts_lower = *it;
				} else {
					in_pts_lower = *(it - 1);
				}
			}
			assert(in_pts >= in_pts_lower);
			assert(in_pts <= in_pts_upper);

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

			if (progress_callback != nullptr) {
				// NOTE: None of this will take into account any snapping done below.
				double played_this_clip = double(in_pts - clip.pts_in) / TIMEBASE / speed;
				double total_length = double(clip.pts_out - clip.pts_in) / TIMEBASE / speed;
				progress_callback(played_this_clip, total_length);
			}

			if (in_pts_lower == in_pts_upper) {
				destination->setFrame(stream_idx, in_pts_lower, /*interpolated=*/false);
				if (video_stream != nullptr) {
					video_stream->schedule_original_frame(lrint(out_pts), stream_idx, in_pts_lower);
				}
				continue;
			}

			// Snap to input frame: If we can do so with less than 1% jitter
			// (ie., move less than 1% of an _output_ frame), do so.
			double in_pts_lower_as_frameno = (in_pts_lower - in_pts_origin) * output_framerate / TIMEBASE / speed;
			double in_pts_upper_as_frameno = (in_pts_upper - in_pts_origin) * output_framerate / TIMEBASE / speed;
			if (fabs(in_pts_lower_as_frameno - frameno) < 0.01) {
				destination->setFrame(stream_idx, in_pts_lower, /*interpolated=*/false);
				if (video_stream != nullptr) {
					video_stream->schedule_original_frame(lrint(out_pts), stream_idx, in_pts_lower);
				}
				in_pts_origin += in_pts_lower - in_pts;
				continue;
			} else if (fabs(in_pts_upper_as_frameno - frameno) < 0.01) {
				destination->setFrame(stream_idx, in_pts_upper, /*interpolated=*/false);
				if (video_stream != nullptr) {
					video_stream->schedule_original_frame(lrint(out_pts), stream_idx, in_pts_upper);
				}
				in_pts_origin += in_pts_upper - in_pts;
				continue;
			}

			double alpha = double(in_pts - in_pts_lower) / (in_pts_upper - in_pts_lower);

			if (video_stream == nullptr) {
				// Previews don't do any interpolation.
				destination->setFrame(stream_idx, in_pts_lower, /*interpolated=*/false);
			} else {
				// Calculate the interpolated frame. When it's done, the destination
				// will be unblocked.
				destination->setFrame(stream_idx, lrint(out_pts), /*interpolated=*/true);
				video_stream->schedule_interpolated_frame(lrint(out_pts), stream_idx, in_pts_lower, in_pts_upper, alpha);
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
	destination->setFrame(stream_idx, *it, /*interpolated=*/false);
}
