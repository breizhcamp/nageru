#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <stdio.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include "clip_list.h"
#include "defs.h"
#include "ffmpeg_raii.h"
#include "httpd.h"
#include "jpeg_frame_view.h"
#include "mux.h"
#include "player.h"

using namespace std;
using namespace std::chrono;

extern mutex frame_mu;
extern vector<int64_t> frames[MAX_STREAMS];
extern HTTPD *global_httpd;

namespace {

string read_file(const string &filename)
{
	FILE *fp = fopen(filename.c_str(), "rb");
	if (fp == nullptr) {
		perror(filename.c_str());
		return "";
	}

	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	rewind(fp);

	string ret;
	ret.resize(len);
	fread(&ret[0], len, 1, fp);
	fclose(fp);
	return ret;
}

}  // namespace

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

			// Send the frame to the stream.
			// FIXME: Vaguely less crazy pts, perhaps.
			double pts_float = fmod(duration<double>(next_frame_start.time_since_epoch()).count(), 86400.0f);
			int64_t pts = lrint(pts_float * TIMEBASE);
			string jpeg = read_file(filename_for_frame(stream_idx, next_pts));
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)jpeg.data();
			pkt.size = jpeg.size();
			stream_mux->add_packet(pkt, pts, pts);
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
	open_output_stream();
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

void Player::open_output_stream()
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = av_guess_format("nut", nullptr, nullptr);

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &Player::write_packet2_thunk;
	avctx->pb->ignore_boundary_point = 1;

	Mux::Codec video_codec = Mux::CODEC_MJPEG;

	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	string video_extradata;

	constexpr int width = 1280, height = 720;  // Doesn't matter for MJPEG.
	stream_mux.reset(new Mux(avctx, width, height, video_codec, video_extradata, /*audio_codec_parameters=*/nullptr, COARSE_TIMEBASE,
		/*write_callback=*/nullptr, Mux::WRITE_FOREGROUND, {}));
}

int Player::write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
        Player *player = (Player *)opaque;
        return player->write_packet2(buf, buf_size, type, time);
}

int Player::write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
        if (type == AVIO_DATA_MARKER_SYNC_POINT || type == AVIO_DATA_MARKER_BOUNDARY_POINT) {
                seen_sync_markers = true;
        } else if (type == AVIO_DATA_MARKER_UNKNOWN && !seen_sync_markers) {
                // We don't know if this is a keyframe or not (the muxer could
                // avoid marking it), so we just have to make the best of it.
                type = AVIO_DATA_MARKER_SYNC_POINT;
        }

        if (type == AVIO_DATA_MARKER_HEADER) {
                stream_mux_header.append((char *)buf, buf_size);
                global_httpd->set_header(stream_mux_header);
        } else {
                global_httpd->add_data((char *)buf, buf_size, type == AVIO_DATA_MARKER_SYNC_POINT, time, AVRational{ AV_TIME_BASE, 1 });
        }
        return buf_size;
}
