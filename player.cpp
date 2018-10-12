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
	Clip next_clip;
	bool got_next_clip = false;
	double next_clip_fade_time = -1.0;

	for ( ;; ) {
		bool clip_ready;
		steady_clock::time_point before_sleep = steady_clock::now();

		// Wait until we're supposed to play something.
		{
			unique_lock<mutex> lock(queue_state_mu);
			clip_ready = new_clip_changed.wait_for(lock, milliseconds(100), [this]{
				return new_clip_ready && current_clip.pts_in != -1;
			});
			new_clip_ready = false;
			playing = true;
		}

		steady_clock::duration time_slept = steady_clock::now() - before_sleep;
		pts += duration_cast<duration<size_t, TimebaseRatio>>(time_slept).count();

		if (!clip_ready) {
			if (video_stream != nullptr) {
				video_stream->schedule_refresh_frame(pts);
			}
			continue;
		}

		Clip clip;
		unsigned stream_idx;
		{
			lock_guard<mutex> lock(mu);
			clip = current_clip;
			stream_idx = current_stream_idx;
		}
got_clip:
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

		for (int frameno = 0; ; ++frameno) {  // Ends when the clip ends.
			double out_pts = out_pts_origin + TIMEBASE * frameno / output_framerate;
			steady_clock::time_point next_frame_start =
				origin + microseconds(lrint((out_pts - out_pts_origin) * 1e6 / TIMEBASE));
			int64_t in_pts = lrint(in_pts_origin + TIMEBASE * frameno * speed / output_framerate);
			pts = lrint(out_pts);

			steady_clock::duration time_behind = steady_clock::now() - next_frame_start;
			if (time_behind >= milliseconds(200)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping a frame (no matter the type).\n",
					lrint(1e3 * duration<double>(time_behind).count()));
				continue;
			}

			double time_left_this_clip = double(clip.pts_out - in_pts) / TIMEBASE / speed;
			if (!got_next_clip && next_clip_callback != nullptr && time_left_this_clip <= clip.fade_time_seconds) {
				// Find the next clip so that we can begin a fade.
				next_clip = next_clip_callback();
				if (next_clip.pts_in != -1) {
					got_next_clip = true;

					double duration_next_clip = (next_clip.pts_out - next_clip.pts_in) / TIMEBASE / speed;
					next_clip_fade_time = std::min(time_left_this_clip, duration_next_clip);
				}
			}

			int primary_stream_idx = stream_idx;
			int secondary_stream_idx = -1;
			float fade_alpha = 0.0f;
			if (got_next_clip) {
				secondary_stream_idx = next_clip.stream_idx;
				fade_alpha = 1.0f - time_left_this_clip / next_clip_fade_time;

				// If more than half-way through the fade, interpolate the next clip
				// instead of the current one, since it's more visible.
				if (fade_alpha >= 0.5f) {
					swap(primary_stream_idx, secondary_stream_idx);
					fade_alpha = 1.0f - fade_alpha;
				}
			}

			int64_t secondary_pts = -1;
			if (got_next_clip) {
				int64_t in_pts_lower, in_pts_upper;
				bool ok = find_surrounding_frames(in_pts, secondary_stream_idx, &in_pts_lower, &in_pts_upper);
				if (ok) {
					secondary_pts = in_pts_lower;
				} else {
					secondary_stream_idx = -1;
				}
			}

			if (progress_callback != nullptr) {
				// NOTE: None of this will take into account any snapping done below.
				double played_this_clip = double(in_pts - clip.pts_in) / TIMEBASE / speed;
				double total_length = double(clip.pts_out - clip.pts_in) / TIMEBASE / speed;
				progress_callback(played_this_clip, total_length);
			}

			int64_t in_pts_lower, in_pts_upper;
			bool ok = find_surrounding_frames(in_pts, primary_stream_idx, &in_pts_lower, &in_pts_upper);
			if (!ok || in_pts_upper >= clip.pts_out) {
				break;
			}

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

			if (in_pts_lower == in_pts_upper) {
				destination->setFrame(primary_stream_idx, in_pts_lower, /*interpolated=*/false, secondary_stream_idx, secondary_pts, fade_alpha);
				if (video_stream != nullptr) {
					if (secondary_stream_idx == -1) {
						video_stream->schedule_original_frame(pts, primary_stream_idx, in_pts_lower);
					} else {
						video_stream->schedule_faded_frame(pts, primary_stream_idx, in_pts_lower, secondary_stream_idx, secondary_pts, fade_alpha);
					}
				}
				continue;
			}

			// Snap to input frame: If we can do so with less than 1% jitter
			// (ie., move less than 1% of an _output_ frame), do so.
			bool snapped = false;
			for (int64_t snap_pts : { in_pts_lower, in_pts_upper }) {
				double snap_pts_as_frameno = (snap_pts - in_pts_origin) * output_framerate / TIMEBASE / speed;
				if (fabs(snap_pts_as_frameno - frameno) < 0.01) {
					destination->setFrame(primary_stream_idx, snap_pts, /*interpolated=*/false, secondary_stream_idx, secondary_pts, fade_alpha);
					if (video_stream != nullptr) {
						if (secondary_stream_idx == -1) {
							video_stream->schedule_original_frame(pts, primary_stream_idx, snap_pts);
						} else {
							video_stream->schedule_faded_frame(pts, primary_stream_idx, snap_pts, secondary_stream_idx, secondary_pts, fade_alpha);
						}
					}
					in_pts_origin += snap_pts - in_pts;
					snapped = true;
					break;
				}
			}
			if (snapped) {
				continue;
			}

			if (time_behind >= milliseconds(100)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping an interpolated frame.\n",
					lrint(1e3 * duration<double>(time_behind).count()));
				continue;
			}

			double alpha = double(in_pts - in_pts_lower) / (in_pts_upper - in_pts_lower);

			if (video_stream == nullptr) {
				// Previews don't do any interpolation.
				assert(secondary_stream_idx == -1);
				destination->setFrame(primary_stream_idx, in_pts_lower, /*interpolated=*/false);
			} else {
				// Calculate the interpolated frame. When it's done, the destination
				// will be unblocked.
				destination->setFrame(primary_stream_idx, pts, /*interpolated=*/true, secondary_stream_idx, secondary_pts, fade_alpha);
				video_stream->schedule_interpolated_frame(pts, primary_stream_idx, in_pts_lower, in_pts_upper, alpha, secondary_stream_idx, secondary_pts, fade_alpha);
			}
		}

		// The clip ended.

		// Last-ditch effort to get the next clip (if e.g. the fade time was zero seconds).
		if (!got_next_clip && next_clip_callback != nullptr) {
			next_clip = next_clip_callback();
			if (next_clip.pts_in != -1) {
				got_next_clip = true;
			}
		}

		// Switch to next clip if we got it.
		if (got_next_clip) {
			clip = next_clip;
			stream_idx = next_clip.stream_idx;  // Override is used for previews only, and next_clip is used for live ony.
			if (done_callback != nullptr) {
				done_callback();
			}
			got_next_clip = false;
			goto got_clip;
		}

		{
			unique_lock<mutex> lock(queue_state_mu);
			playing = false;
		}
		if (done_callback != nullptr) {
			done_callback();
		}
	}
}

// Find the frame immediately before and after this point.
bool Player::find_surrounding_frames(int64_t pts, int stream_idx, int64_t *pts_lower, int64_t *pts_upper)
{
	lock_guard<mutex> lock(frame_mu);

	// Find the first frame such that frame.pts >= pts.
	auto it = lower_bound(frames[stream_idx].begin(),
		frames[stream_idx].end(),
		pts);
	if (it == frames[stream_idx].end()) {
		return false;
	}
	*pts_upper = *it;

	// Find the last frame such that in_pts <= frame.pts (if any).
	if (it == frames[stream_idx].begin()) {
		*pts_lower = *it;
	} else {
		*pts_lower = *(it - 1);
	}
	assert(pts >= *pts_lower);
	assert(pts <= *pts_upper);
	return true;
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
