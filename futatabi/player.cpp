#include "player.h"

#include "clip_list.h"
#include "defs.h"
#include "flags.h"
#include "frame_on_disk.h"
#include "jpeg_frame_view.h"
#include "shared/context.h"
#include "shared/ffmpeg_raii.h"
#include "shared/httpd.h"
#include "shared/metrics.h"
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

void Player::thread_func(AVFormatContext *file_avctx)
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

	while (!should_quit) {
		play_playlist_once();
		if (done_callback != nullptr) {
			done_callback();
		}
	}
}

namespace {

double calc_progress(const Clip &clip, int64_t pts)
{
	return double(pts - clip.pts_in) / (clip.pts_out - clip.pts_in);
}

}  // namespace

void Player::play_playlist_once()
{
	vector<ClipWithRow> clip_list;
	bool clip_ready;
	steady_clock::time_point before_sleep = steady_clock::now();

	// Wait until we're supposed to play something.
	{
		unique_lock<mutex> lock(queue_state_mu);
		playing = false;
		clip_ready = new_clip_changed.wait_for(lock, milliseconds(100), [this] {
			return should_quit || new_clip_ready;
		});
		if (should_quit) {
			return;
		}
		if (clip_ready) {
			new_clip_ready = false;
			playing = true;
			clip_list = move(queued_clip_list);
			queued_clip_list.clear();
			assert(!clip_list.empty());
		}
	}

	steady_clock::duration time_slept = steady_clock::now() - before_sleep;
	pts += duration_cast<duration<size_t, TimebaseRatio>>(time_slept).count();

	if (!clip_ready) {
		if (video_stream != nullptr) {
			++metric_refresh_frame;
			video_stream->schedule_refresh_frame(steady_clock::now(), pts, /*display_func=*/nullptr, QueueSpotHolder());
		}
		return;
	}

	steady_clock::time_point origin = steady_clock::now();  // TODO: Add a 100 ms buffer for ramp-up?
	int64_t in_pts_origin = clip_list[0].clip.pts_in;
	for (size_t clip_idx = 0; clip_idx < clip_list.size(); ++clip_idx) {
		const Clip &clip = clip_list[clip_idx].clip;
		const Clip *next_clip = (clip_idx + 1 < clip_list.size()) ? &clip_list[clip_idx + 1].clip : nullptr;
		int64_t out_pts_origin = pts;

		double next_clip_fade_time = -1.0;
		if (next_clip != nullptr) {
			double duration_this_clip = double(clip.pts_out - in_pts_origin) / TIMEBASE / clip.speed;
			double duration_next_clip = double(next_clip->pts_out - next_clip->pts_in) / TIMEBASE / clip.speed;
			next_clip_fade_time = min(min(duration_this_clip, duration_next_clip), clip.fade_time_seconds);
		}

		int stream_idx = clip.stream_idx;

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

		steady_clock::time_point next_frame_start;
		for (int frameno = 0; !should_quit; ++frameno) {  // Ends when the clip ends.
			double out_pts = out_pts_origin + TIMEBASE * frameno / global_flags.output_framerate;
			next_frame_start =
				origin + microseconds(lrint((out_pts - out_pts_origin) * 1e6 / TIMEBASE));
			int64_t in_pts = lrint(in_pts_origin + TIMEBASE * frameno * clip.speed / global_flags.output_framerate);
			pts = lrint(out_pts);

			if (in_pts >= clip.pts_out) {
				break;
			}

			steady_clock::duration time_behind = steady_clock::now() - next_frame_start;
			if (stream_output != FILE_STREAM_OUTPUT && time_behind >= milliseconds(200)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping a frame (no matter the type).\n",
				        lrint(1e3 * duration<double>(time_behind).count()));
				++metric_dropped_unconditional_frame;
				continue;
			}

			// pts not affected by the swapping below.
			int64_t in_pts_for_progress = in_pts, in_pts_secondary_for_progress = -1;

			int primary_stream_idx = stream_idx;
			FrameOnDisk secondary_frame;
			int secondary_stream_idx = -1;
			float fade_alpha = 0.0f;
			double time_left_this_clip = double(clip.pts_out - in_pts) / TIMEBASE / clip.speed;
			if (next_clip != nullptr && time_left_this_clip <= next_clip_fade_time) {
				// We're in a fade to the next clip.
				secondary_stream_idx = next_clip->stream_idx;
				int64_t in_pts_secondary = lrint(next_clip->pts_in + (next_clip_fade_time - time_left_this_clip) * TIMEBASE * clip.speed);
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
				double clip_progress = calc_progress(clip, in_pts_for_progress);
				map<size_t, double> progress{ { clip_list[clip_idx].row, clip_progress } };
				double time_remaining;
				if (next_clip != nullptr && time_left_this_clip <= next_clip_fade_time) {
					double next_clip_progress = calc_progress(*next_clip, in_pts_secondary_for_progress);
					progress[clip_list[clip_idx + 1].row] = next_clip_progress;
					time_remaining = compute_time_left(clip_list, clip_idx + 1, next_clip_progress);
				} else {
					time_remaining = compute_time_left(clip_list, clip_idx, clip_progress);
				}
				progress_callback(progress, time_remaining);
			}

			FrameOnDisk frame_lower, frame_upper;
			bool ok = find_surrounding_frames(in_pts, primary_stream_idx, &frame_lower, &frame_upper);
			if (!ok) {
				break;
			}

			// Wait until we should, or (given buffering) can, output the frame.
			{
				unique_lock<mutex> lock(queue_state_mu);
				if (video_stream == nullptr) {
					// No queue, just wait until the right time and then show the frame.
					new_clip_changed.wait_until(lock, next_frame_start, [this] {
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
					new_clip_changed.wait(lock, [this] {
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
					return;
				}
				// Honor if we got an override request for the camera.
				if (override_stream_idx != -1) {
					stream_idx = override_stream_idx;
					override_stream_idx = -1;
					continue;
				}
			}

			// If there's nothing to interpolate between, or if interpolation is turned off,
			// or we're a preview, then just display the frame.
			if (frame_lower.pts == frame_upper.pts || global_flags.interpolation_quality == 0 || video_stream == nullptr) {
				display_single_frame(primary_stream_idx, frame_lower, secondary_stream_idx,
				                     secondary_frame, fade_alpha, next_frame_start, /*snapped=*/false);
				continue;
			}

			// Snap to input frame: If we can do so with less than 1% jitter
			// (ie., move less than 1% of an _output_ frame), do so.
			// TODO: Snap secondary (fade-to) clips in the same fashion.
			double pts_snap_tolerance = 0.01 * double(TIMEBASE) / global_flags.output_framerate;
			bool snapped = false;
			for (FrameOnDisk snap_frame : { frame_lower, frame_upper }) {
				if (fabs(snap_frame.pts - in_pts) < pts_snap_tolerance) {
					display_single_frame(primary_stream_idx, snap_frame, secondary_stream_idx,
					                     secondary_frame, fade_alpha, next_frame_start, /*snapped=*/true);
					in_pts_origin += snap_frame.pts - in_pts;
					snapped = true;
					break;
				}
			}
			if (snapped) {
				continue;
			}

			// The snapping above makes us lock to the input framerate, even in the presence
			// of pts drift, for most typical cases where it's needed, like converting 60 → 2x60
			// or 60 → 2x59.94. However, there are some corner cases like 25 → 2x59.94, where we'd
			// get a snap very rarely (in the given case, once every 24 output frames), and by
			// that time, we'd have drifted out. We could have solved this by changing the overall
			// speed ever so slightly, but it requires that we know the actual frame rate (which
			// is difficult in the presence of jitter and missed frames), or at least do some kind
			// of matching/clustering. Instead, we take the opportunity to lock to in-between rational
			// points if we can. E.g., if we are converting 60 → 2x60, we would not only snap to
			// an original frame every other frame; we would also snap to exactly alpha=0.5 every
			// in-between frame. Of course, we will still need to interpolate, but we get a lot
			// closer when we actually get close to an original frame. In other words: Snap more
			// often, but snap less each time. Unless the input and output frame rates are completely
			// decorrelated with no common factor, of course (e.g. 12.345 → 34.567, which we should
			// really never see in practice).
			for (double fraction : { 1.0 / 2.0, 1.0 / 3.0, 2.0 / 3.0, 1.0 / 4.0, 3.0 / 4.0,
			                         1.0 / 5.0, 2.0 / 5.0, 3.0 / 5.0, 4.0 / 5.0 }) {
				double subsnap_pts = frame_lower.pts + fraction * (frame_upper.pts - frame_lower.pts);
				if (fabs(subsnap_pts - in_pts) < pts_snap_tolerance) {
					in_pts_origin += lrint(subsnap_pts) - in_pts;
					in_pts = lrint(subsnap_pts);
					break;
				}
			}

			if (stream_output != FILE_STREAM_OUTPUT && time_behind >= milliseconds(100)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping an interpolated frame.\n",
				        lrint(1e3 * duration<double>(time_behind).count()));
				++metric_dropped_interpolated_frame;
				continue;
			}

			double alpha = double(in_pts - frame_lower.pts) / (frame_upper.pts - frame_lower.pts);
			auto display_func = [this](shared_ptr<Frame> frame) {
				if (destination != nullptr) {
					destination->setFrame(frame);
				}
			};
			if (secondary_stream_idx == -1) {
				++metric_interpolated_frame;
			} else {
				++metric_interpolated_faded_frame;
			}
			video_stream->schedule_interpolated_frame(
				next_frame_start, pts, display_func, QueueSpotHolder(this),
				frame_lower, frame_upper, alpha,
				secondary_frame, fade_alpha);
			last_pts_played = in_pts;  // Not really needed; only previews use last_pts_played.
		}

		// The clip ended.
		if (should_quit) {
			return;
		}

		// Start the next clip from the point where the fade went out.
		if (next_clip != nullptr) {
			origin = next_frame_start;
			in_pts_origin = next_clip->pts_in + lrint(next_clip_fade_time * TIMEBASE * clip.speed);
		}
	}
}

void Player::display_single_frame(int primary_stream_idx, const FrameOnDisk &primary_frame, int secondary_stream_idx, const FrameOnDisk &secondary_frame, double fade_alpha, steady_clock::time_point frame_start, bool snapped)
{
	auto display_func = [this, primary_stream_idx, primary_frame, secondary_frame, fade_alpha] {
		if (destination != nullptr) {
			destination->setFrame(primary_stream_idx, primary_frame, secondary_frame, fade_alpha);
		}
	};
	if (video_stream == nullptr) {
		display_func();
	} else {
		if (secondary_stream_idx == -1) {
			// NOTE: We could be increasing unused metrics for previews, but that's harmless.
			if (snapped) {
				++metric_original_snapped_frame;
			} else {
				++metric_original_frame;
			}
			video_stream->schedule_original_frame(
				frame_start, pts, display_func, QueueSpotHolder(this),
				primary_frame);
		} else {
			assert(secondary_frame.pts != -1);
			// NOTE: We could be increasing unused metrics for previews, but that's harmless.
			if (snapped) {
				++metric_faded_snapped_frame;
			} else {
				++metric_faded_frame;
			}
			video_stream->schedule_faded_frame(frame_start, pts, display_func,
			                                   QueueSpotHolder(this), primary_frame,
			                                   secondary_frame, fade_alpha);
		}
	}
	last_pts_played = primary_frame.pts;
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
	: destination(destination), stream_output(stream_output)
{
	player_thread = thread(&Player::thread_func, this, file_avctx);

	if (stream_output == HTTPD_STREAM_OUTPUT) {
		global_metrics.add("http_output_frames", { { "type", "original" }, { "reason", "edge_frame_or_no_interpolation" } }, &metric_original_frame);
		global_metrics.add("http_output_frames", { { "type", "faded" }, { "reason", "edge_frame_or_no_interpolation" } }, &metric_faded_frame);
		global_metrics.add("http_output_frames", { { "type", "original" }, { "reason", "snapped" } }, &metric_original_snapped_frame);
		global_metrics.add("http_output_frames", { { "type", "faded" }, { "reason", "snapped" } }, &metric_faded_snapped_frame);
		global_metrics.add("http_output_frames", { { "type", "interpolated" } }, &metric_interpolated_frame);
		global_metrics.add("http_output_frames", { { "type", "interpolated_faded" } }, &metric_interpolated_faded_frame);
		global_metrics.add("http_output_frames", { { "type", "refresh" } }, &metric_refresh_frame);
		global_metrics.add("http_dropped_frames", { { "type", "interpolated" } }, &metric_dropped_interpolated_frame);
		global_metrics.add("http_dropped_frames", { { "type", "unconditional" } }, &metric_dropped_unconditional_frame);
	}
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

void Player::play(const vector<Player::ClipWithRow> &clips)
{
	lock_guard<mutex> lock(queue_state_mu);
	new_clip_ready = true;
	queued_clip_list = clips;
	override_stream_idx = -1;
	new_clip_changed.notify_all();
}

void Player::override_angle(unsigned stream_idx)
{
	int64_t last_pts;

	// Corner case: If a new clip is waiting to be played, change its stream and then we're done.
	{
		lock_guard<mutex> lock(queue_state_mu);
		if (new_clip_ready) {
			assert(queued_clip_list.size() == 1);
			queued_clip_list[0].clip.stream_idx = stream_idx;
			return;
		}

		// If we are playing a clip, set override_stream_idx, and the player thread will
		// pick it up and change its internal index.
		if (playing) {
			override_stream_idx = stream_idx;
			new_clip_changed.notify_all();
			return;
		}

		// OK, so we're standing still, presumably at the end of a clip.
		// Look at the last frame played (if it exists), and show the closest
		// thing we've got.
		if (last_pts_played < 0) {
			return;
		}
		last_pts = last_pts_played;
	}

	lock_guard<mutex> lock(frame_mu);
	auto it = find_first_frame_at_or_after(frames[stream_idx], last_pts);
	if (it == frames[stream_idx].end()) {
		return;
	}
	destination->setFrame(stream_idx, *it);
}

void Player::take_queue_spot()
{
	lock_guard<mutex> lock(queue_state_mu);
	++num_queued_frames;
}

void Player::release_queue_spot()
{
	lock_guard<mutex> lock(queue_state_mu);
	assert(num_queued_frames > 0);
	--num_queued_frames;
	new_clip_changed.notify_all();
}

double compute_time_left(const vector<Player::ClipWithRow> &clips, size_t currently_playing_idx, double progress_currently_playing) 
{
	// Look at the last clip and then start counting from there.
	double remaining = 0.0;
	double last_fade_time_seconds = 0.0;
	for (size_t row = currently_playing_idx; row < clips.size(); ++row) {
		const Clip &clip = clips[row].clip;
		double clip_length = double(clip.pts_out - clip.pts_in) / TIMEBASE / clip.speed;
		if (row == currently_playing_idx) {
			// A clip we're playing: Subtract the part we've already played.
			remaining = clip_length * (1.0 - progress_currently_playing);
		} else {
			// A clip we haven't played yet: Subtract the part that's overlapping
			// with a previous clip (due to fade).
			remaining += max(clip_length - last_fade_time_seconds, 0.0);
		}
		last_fade_time_seconds = min(clip_length, clip.fade_time_seconds);
	}
	return remaining;
}
