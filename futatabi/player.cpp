#include "player.h"

#include "clip_list.h"
#include "shared/context.h"
#include "defs.h"
#include "shared/ffmpeg_raii.h"
#include "flags.h"
#include "frame_on_disk.h"
#include "shared/httpd.h"
#include "jpeg_frame_view.h"
#include "shared/mux.h"
#include "shared/timebase.h"
#include "video_stream.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <movit/util.h>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <vector>

using namespace std;
using namespace std::chrono;

extern HTTPD *global_httpd;

void Player::thread_func(Player::StreamOutput stream_output, AVFormatContext *file_avctx)
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
	if (stream_output != NO_STREAM_OUTPUT) {
		video_stream.reset(new VideoStream(file_avctx));
		video_stream->start();
	}

	check_error();

	int64_t pts = 0;
	Clip next_clip;
	size_t next_clip_idx = size_t(-1);
	bool got_next_clip = false;
	double next_clip_fade_time = -1.0;

	while (!should_quit) {
wait_for_clip:
		bool clip_ready;
		steady_clock::time_point before_sleep = steady_clock::now();

		// Wait until we're supposed to play something.
		{
			unique_lock<mutex> lock(queue_state_mu);
			clip_ready = new_clip_changed.wait_for(lock, milliseconds(100), [this] {
				return should_quit || (new_clip_ready && current_clip.pts_in != -1);
			});
			if (should_quit) {
				return;
			}
			new_clip_ready = false;
			playing = true;
		}

		steady_clock::duration time_slept = steady_clock::now() - before_sleep;
		pts += duration_cast<duration<size_t, TimebaseRatio>>(time_slept).count();

		if (!clip_ready) {
			if (video_stream != nullptr) {
				video_stream->schedule_refresh_frame(steady_clock::now(), pts, /*display_func=*/nullptr, QueueSpotHolder());
			}
			continue;
		}

		Clip clip;
		size_t clip_idx;
		unsigned stream_idx;
		{
			lock_guard<mutex> lock(mu);
			clip = current_clip;
			clip_idx = current_clip_idx;
			stream_idx = current_stream_idx;
		}
		steady_clock::time_point origin = steady_clock::now();  // TODO: Add a 100 ms buffer for ramp-up?
		int64_t in_pts_origin = clip.pts_in;
got_clip:
		int64_t out_pts_origin = pts;

		// Start playing exactly at a frame.
		// TODO: Snap secondary (fade-to) clips in the same fashion
		// so that we don't get jank here).
		{
			lock_guard<mutex> lock(frame_mu);

			// Find the first frame such that frame.pts <= in_pts.
			auto it = find_last_frame_before(frames[stream_idx], in_pts_origin);
			if (it != frames[stream_idx].end()) {
				in_pts_origin = it->pts;
			}
		}

		// TODO: Lock to a rational multiple of the frame rate if possible.
		double speed = 0.5;

		int64_t in_pts_start_next_clip = -1;
		steady_clock::time_point next_frame_start;
		for (int frameno = 0; !should_quit; ++frameno) {  // Ends when the clip ends.
			double out_pts = out_pts_origin + TIMEBASE * frameno / global_flags.output_framerate;
			next_frame_start =
				origin + microseconds(lrint((out_pts - out_pts_origin) * 1e6 / TIMEBASE));
			int64_t in_pts = lrint(in_pts_origin + TIMEBASE * frameno * speed / global_flags.output_framerate);
			pts = lrint(out_pts);

			if (in_pts >= clip.pts_out) {
				break;
			}

			steady_clock::duration time_behind = steady_clock::now() - next_frame_start;
			if (stream_output != FILE_STREAM_OUTPUT && time_behind >= milliseconds(200)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping a frame (no matter the type).\n",
					lrint(1e3 * duration<double>(time_behind).count()));
				continue;
			}

			double time_left_this_clip = double(clip.pts_out - in_pts) / TIMEBASE / speed;
			if (!got_next_clip && next_clip_callback != nullptr && time_left_this_clip <= clip.fade_time_seconds) {
				// Find the next clip so that we can begin a fade.
				tie(next_clip, next_clip_idx) = next_clip_callback();
				if (next_clip.pts_in != -1) {
					got_next_clip = true;

					double duration_next_clip = (next_clip.pts_out - next_clip.pts_in) / TIMEBASE / speed;
					next_clip_fade_time = std::min(time_left_this_clip, duration_next_clip);
					in_pts_start_next_clip = next_clip.pts_in + lrint(next_clip_fade_time * TIMEBASE * speed);
				}
			}

			// pts not affected by the swapping below.
			int64_t in_pts_for_progress = in_pts, in_pts_secondary_for_progress = -1;

			int primary_stream_idx = stream_idx;
			FrameOnDisk secondary_frame;
			int secondary_stream_idx = -1;
			float fade_alpha = 0.0f;
			if (got_next_clip && time_left_this_clip <= next_clip_fade_time) {
				secondary_stream_idx = next_clip.stream_idx;
				int64_t in_pts_secondary = lrint(next_clip.pts_in + (next_clip_fade_time - time_left_this_clip) * TIMEBASE * speed);
				in_pts_secondary_for_progress = in_pts_secondary;
				fade_alpha = 1.0f - time_left_this_clip / next_clip_fade_time;

				// If more than half-way through the fade, interpolate the next clip
				// instead of the current one, since it's more visible.
				if (fade_alpha >= 0.5f) {
					swap(primary_stream_idx, secondary_stream_idx);
					swap(in_pts, in_pts_secondary);
					fade_alpha = 1.0f - fade_alpha;
				}

				FrameOnDisk frame_lower, frame_upper;
				bool ok = find_surrounding_frames(in_pts_secondary, secondary_stream_idx, &frame_lower, &frame_upper);
				if (ok) {
					secondary_frame = frame_lower;
				}
			}

			if (progress_callback != nullptr) {
				// NOTE: None of this will take into account any snapping done below.
				double played_this_clip = double(in_pts_for_progress - clip.pts_in) / TIMEBASE / speed;
				double total_length = double(clip.pts_out - clip.pts_in) / TIMEBASE / speed;
				map<size_t, double> progress{{ clip_idx, played_this_clip / total_length }};

				if (got_next_clip && time_left_this_clip <= next_clip_fade_time) {
					double played_next_clip = double(in_pts_secondary_for_progress - next_clip.pts_in) / TIMEBASE / speed;
					double total_next_length = double(next_clip.pts_out - next_clip.pts_in) / TIMEBASE / speed;
					progress[next_clip_idx] = played_next_clip / total_next_length;
				}
				progress_callback(progress);
			}

			FrameOnDisk frame_lower, frame_upper;
			bool ok = find_surrounding_frames(in_pts, primary_stream_idx, &frame_lower, &frame_upper);
			if (!ok) {
				break;
			}

			{
				unique_lock<mutex> lock(queue_state_mu);
				if (video_stream == nullptr) {
					// No queue, just wait until the right time and then show the frame.
					new_clip_changed.wait_until(lock, next_frame_start, [this]{
						return should_quit || new_clip_ready || override_stream_idx != -1;
					});
				if (should_quit) {
					return;
				}
				} else {
					// If the queue is full (which is really the state we'd like to be in),
					// wait until there's room for one more frame (ie., one was output from
					// VideoStream), or until or until there's a new clip we're supposed to play.
					//
					// In this case, we don't sleep until next_frame_start; the displaying is
					// done by the queue.
					new_clip_changed.wait(lock, [this]{
						if (num_queued_frames < max_queued_frames) {
							return true;
						}
						return should_quit || new_clip_ready || override_stream_idx != -1;
					});
				}
				if (should_quit) {
					return;
				}
				if (new_clip_ready) {
					if (video_stream != nullptr) {
						lock.unlock();  // Urg.
						video_stream->clear_queue();
						lock.lock();
					}
					got_next_clip = false;
					goto wait_for_clip;
				}
				if (override_stream_idx != -1) {
					stream_idx = override_stream_idx;
					override_stream_idx = -1;
					continue;
				}
			}

			if (frame_lower.pts == frame_upper.pts || global_flags.interpolation_quality == 0) {
				auto display_func = [this, primary_stream_idx, frame_lower, secondary_frame, fade_alpha]{
					if (destination != nullptr) {
						destination->setFrame(primary_stream_idx, frame_lower, secondary_frame, fade_alpha);
					}
				};
				if (video_stream == nullptr) {
					display_func();
				} else {
					if (secondary_stream_idx == -1) {
						video_stream->schedule_original_frame(
							next_frame_start, pts, display_func, QueueSpotHolder(this),
							frame_lower);
					} else {
						assert(secondary_frame.pts != -1);
						video_stream->schedule_faded_frame(next_frame_start, pts, display_func,
							QueueSpotHolder(this), frame_lower,
							secondary_frame, fade_alpha);
					}
				}
				continue;
			}

			// Snap to input frame: If we can do so with less than 1% jitter
			// (ie., move less than 1% of an _output_ frame), do so.
			// TODO: Snap secondary (fade-to) clips in the same fashion.
			bool snapped = false;
			for (FrameOnDisk snap_frame : { frame_lower, frame_upper }) {
				double snap_pts_as_frameno = (snap_frame.pts - in_pts_origin) * global_flags.output_framerate / TIMEBASE / speed;
				if (fabs(snap_pts_as_frameno - frameno) < 0.01) {
					auto display_func = [this, primary_stream_idx, snap_frame, secondary_frame, fade_alpha]{
						if (destination != nullptr) {
							destination->setFrame(primary_stream_idx, snap_frame, secondary_frame, fade_alpha);
						}
					};
					if (video_stream == nullptr) {
						display_func();
					} else {
						if (secondary_stream_idx == -1) {
							video_stream->schedule_original_frame(
								next_frame_start, pts, display_func,
								QueueSpotHolder(this), snap_frame);
						} else {
							assert(secondary_frame.pts != -1);
							video_stream->schedule_faded_frame(
								next_frame_start, pts, display_func, QueueSpotHolder(this),
								snap_frame, secondary_frame, fade_alpha);
						}
					}
					in_pts_origin += snap_frame.pts - in_pts;
					snapped = true;
					break;
				}
			}
			if (snapped) {
				continue;
			}

			if (stream_output != FILE_STREAM_OUTPUT && time_behind >= milliseconds(100)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping an interpolated frame.\n",
					lrint(1e3 * duration<double>(time_behind).count()));
				continue;
			}

			double alpha = double(in_pts - frame_lower.pts) / (frame_upper.pts - frame_lower.pts);

			if (video_stream == nullptr) {
				// Previews don't do any interpolation.
				assert(secondary_stream_idx == -1);
				if (destination != nullptr) {
					destination->setFrame(primary_stream_idx, frame_lower);
				}
			} else {
				auto display_func = [this](shared_ptr<Frame> frame) {
					if (destination != nullptr) {
						destination->setFrame(frame);
					}
				};
				video_stream->schedule_interpolated_frame(
					next_frame_start, pts, display_func, QueueSpotHolder(this),
					frame_lower, frame_upper, alpha,
					secondary_frame, fade_alpha);
			}
		}

		if (should_quit) {
			return;
		}

		// The clip ended.

		// Last-ditch effort to get the next clip (if e.g. the fade time was zero seconds).
		if (!got_next_clip && next_clip_callback != nullptr) {
			tie(next_clip, next_clip_idx) = next_clip_callback();
			if (next_clip.pts_in != -1) {
				got_next_clip = true;
				in_pts_start_next_clip = next_clip.pts_in;
			}
		}

		// Switch to next clip if we got it.
		if (got_next_clip) {
			clip = next_clip;
			clip_idx = next_clip_idx;
			stream_idx = next_clip.stream_idx;  // Override is used for previews only, and next_clip is used for live ony.
			if (done_callback != nullptr) {
				done_callback();
			}
			got_next_clip = false;

			// Start the next clip from the point where the fade went out.
			origin = next_frame_start;
			in_pts_origin = in_pts_start_next_clip;
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
bool Player::find_surrounding_frames(int64_t pts, int stream_idx, FrameOnDisk *frame_lower, FrameOnDisk *frame_upper)
{
	lock_guard<mutex> lock(frame_mu);

	// Find the first frame such that frame.pts >= pts.
	auto it = find_last_frame_before(frames[stream_idx], pts);
	if (it == frames[stream_idx].end()) {
		return false;
	}
	*frame_upper = *it;

	// Find the last frame such that in_pts <= frame.pts (if any).
	if (it == frames[stream_idx].begin()) {
		*frame_lower = *it;
	} else {
		*frame_lower = *(it - 1);
	}
	assert(pts >= frame_lower->pts);
	assert(pts <= frame_upper->pts);
	return true;
}

Player::Player(JPEGFrameView *destination, Player::StreamOutput stream_output, AVFormatContext *file_avctx)
	: destination(destination)
{
	player_thread = thread(&Player::thread_func, this, stream_output, file_avctx);
}

Player::~Player()
{
	should_quit = true;
	if (video_stream != nullptr) {
		video_stream->stop();
	}
	new_clip_changed.notify_all();
	player_thread.join();
}

void Player::play_clip(const Clip &clip, size_t clip_idx, unsigned stream_idx)
{
	{
		lock_guard<mutex> lock(mu);
		current_clip = clip;
		current_stream_idx = stream_idx;
		current_clip_idx = clip_idx;
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
	auto it = find_first_frame_at_or_after(frames[stream_idx], pts_out);
	if (it == frames[stream_idx].end()) {
		return;
	}
	destination->setFrame(stream_idx, *it);
}

void Player::take_queue_spot()
{
	unique_lock<mutex> lock(queue_state_mu);
	++num_queued_frames;
}

void Player::release_queue_spot()
{
	unique_lock<mutex> lock(queue_state_mu);
	assert(num_queued_frames > 0);
	--num_queued_frames;
	new_clip_changed.notify_all();
}
