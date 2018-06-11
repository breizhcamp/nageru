#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "clip_list.h"
#include "defs.h"
#include "mainwindow.h"
#include "ffmpeg_raii.h"
#include "post_to_main_thread.h"
#include "ui_mainwindow.h"

using namespace std;
using namespace std::chrono;

extern mutex frame_mu;
extern vector<int64_t> frames[MAX_STREAMS];

struct PlaylistClip {
	Clip clip;
	unsigned stream_idx;
};
vector<PlaylistClip> current_cue_playlist;
mutex playlist_mu;

enum { PAUSED, PLAYING } cue_state = PAUSED;
mutex cue_state_mu;
condition_variable cue_is_playing;
//int cue_playlist_index = -1;
//int64_t cue_playlist_pos = 0;

int preview_thread_func()
{
	for ( ;; ) {
		// Wait until we're supposed to play something.
		{
			unique_lock<mutex> lock(cue_state_mu);
			cue_is_playing.wait(lock, []{
				return cue_state == PLAYING;
				//return current_cue_status.origin != steady_clock::time_point::max();
			});
		}

		PlaylistClip clip;
		{
			lock_guard<mutex> lock2(playlist_mu);
			clip = current_cue_playlist[0];
		}
		steady_clock::time_point origin = steady_clock::now();
		int64_t pts_origin = clip.clip.pts_in;

		int64_t next_pts = pts_origin;

		bool eof = false;
		while (!eof) {  // TODO: check for abort
			// FIXME: assumes a given timebase.
			double speed = 0.5;
			steady_clock::time_point next_frame_start =
				origin + microseconds((next_pts - pts_origin) * int(1000000 / speed) / 12800);
			this_thread::sleep_until(next_frame_start);
			global_mainwindow->ui->preview_display->setFrame(clip.stream_idx, next_pts);

			// Find the next frame.
			{
				lock_guard<mutex> lock2(frame_mu);
				auto it = upper_bound(frames[clip.stream_idx].begin(),
					frames[clip.stream_idx].end(),
					next_pts);
				if (it == frames[clip.stream_idx].end()) {
					eof = true;
				} else {
					next_pts = *it;
					if (next_pts >= clip.clip.pts_out) {
						eof = true;
					}
				}
			}
			if (eof) break;
		}

		// TODO: advance the playlist and look for the next element.
		{
			unique_lock<mutex> lock(cue_state_mu);
			cue_state = PAUSED;
		}
	}
}

void start_player_thread()
{
	thread(preview_thread_func).detach();
}

void play_clip(const Clip &clip, unsigned stream_idx)
{
	{
		lock_guard<mutex> lock(playlist_mu);
		current_cue_playlist.clear();
		current_cue_playlist.push_back(PlaylistClip{ clip, stream_idx });
	}

	{
		lock_guard<mutex> lock(cue_state_mu);
		cue_state = PLAYING;
		cue_is_playing.notify_all();
	}
}
