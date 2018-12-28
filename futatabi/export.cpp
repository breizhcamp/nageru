#include "export.h"

#include "clip_list.h"
#include "defs.h"
#include "flags.h"
#include "frame_on_disk.h"
#include "player.h"
#include "shared/ffmpeg_raii.h"
#include "shared/timebase.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <future>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace std;

namespace {

// Only used in export_cliplist_clip_multitrack_triggered.
struct BufferedJPEG {
	int64_t pts;
	unsigned stream_idx;
	string jpeg;
};

bool write_buffered_jpegs(AVFormatContext *avctx, const vector<BufferedJPEG> &buffered_jpegs)
{
	for (const BufferedJPEG &jpeg : buffered_jpegs) {
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.stream_index = jpeg.stream_idx;
		pkt.data = (uint8_t *)jpeg.jpeg.data();
		pkt.size = jpeg.jpeg.size();
		pkt.pts = jpeg.pts;
		pkt.dts = jpeg.pts;
		pkt.flags = AV_PKT_FLAG_KEY;

		if (av_write_frame(avctx, &pkt) < 0) {
			return false;
		}
	}
	return true;
}

}  // namespace

void export_multitrack_clip(const string &filename, const Clip &clip)
{
	AVFormatContext *avctx = nullptr;
	avformat_alloc_output_context2(&avctx, NULL, NULL, filename.c_str());
	if (avctx == nullptr) {
		QMessageBox msgbox;
		msgbox.setText("Could not allocate FFmpeg context");
		msgbox.exec();
		return;
	}
	AVFormatContextWithCloser closer(avctx);

	int ret = avio_open(&avctx->pb, filename.c_str(), AVIO_FLAG_WRITE);
	if (ret < 0) {
		QMessageBox msgbox;
		msgbox.setText(QString::fromStdString("Could not open output file '" + filename + "'"));
		msgbox.exec();
		return;
	}

	// Find the first frame for each stream.
	size_t num_frames = 0;
	size_t num_streams_with_frames_left = 0;
	size_t last_stream_idx = 0;
	FrameReader readers[MAX_STREAMS];
	bool has_frames[MAX_STREAMS];
	size_t first_frame_idx[MAX_STREAMS], last_frame_idx[MAX_STREAMS];  // Inclusive, exclusive.
	{
		lock_guard<mutex> lock(frame_mu);
		for (size_t stream_idx = 0; stream_idx < MAX_STREAMS; ++stream_idx) {
			// Find the first frame such that frame.pts <= pts_in.
			auto it = find_first_frame_at_or_after(frames[stream_idx], clip.pts_in);
			first_frame_idx[stream_idx] = distance(frames[stream_idx].begin(), it);
			has_frames[stream_idx] = (it != frames[stream_idx].end());

			// Find the first frame such that frame.pts >= pts_out.
			it = find_first_frame_at_or_after(frames[stream_idx], clip.pts_out);
			last_frame_idx[stream_idx] = distance(frames[stream_idx].begin(), it);
			num_frames += last_frame_idx[stream_idx] - first_frame_idx[stream_idx];

			if (has_frames[stream_idx]) {
				++num_streams_with_frames_left;
				last_stream_idx = stream_idx;
			}
		}
	}

	// Create the streams. Note that some of them could be without frames
	// (we try to maintain the stream indexes in the export).
	vector<AVStream *> video_streams;
	for (unsigned stream_idx = 0; stream_idx <= last_stream_idx; ++stream_idx) {
		AVStream *avstream_video = avformat_new_stream(avctx, nullptr);
		if (avstream_video == nullptr) {
			fprintf(stderr, "avformat_new_stream() failed\n");
			exit(1);
		}
		avstream_video->time_base = AVRational{ 1, TIMEBASE };
		avstream_video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
		avstream_video->codecpar->codec_id = AV_CODEC_ID_MJPEG;
		avstream_video->codecpar->width = global_flags.width;  // Might be wrong, but doesn't matter all that much.
		avstream_video->codecpar->height = global_flags.height;

		// TODO: Deduplicate this against Mux.
		avstream_video->codecpar->color_primaries = AVCOL_PRI_BT709;  // RGB colorspace (inout_format.color_space).
		avstream_video->codecpar->color_trc = AVCOL_TRC_IEC61966_2_1;  // Gamma curve (inout_format.gamma_curve).
		// YUV colorspace (output_ycbcr_format.luma_coefficients).
		avstream_video->codecpar->color_space = AVCOL_SPC_BT709;
		avstream_video->codecpar->color_range = AVCOL_RANGE_MPEG;  // Full vs. limited range (output_ycbcr_format.full_range).
		avstream_video->codecpar->chroma_location = AVCHROMA_LOC_LEFT;  // Chroma sample location. See chroma_offset_0[] in Mixer::subsample_chroma().
		avstream_video->codecpar->field_order = AV_FIELD_PROGRESSIVE;
		video_streams.push_back(avstream_video);
	}

	if (avformat_write_header(avctx, nullptr) < 0) {
		QMessageBox msgbox;
		msgbox.setText("Writing header failed");
		msgbox.exec();
		unlink(filename.c_str());
		return;
	}

	QProgressDialog progress(QString::fromStdString("Exporting to " + filename + "..."), "Abort", 0, 1);
	progress.setWindowTitle("Futatabi");
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(1000);
	progress.setMaximum(num_frames);
	progress.setValue(0);

	// We buffer up to 1000 frames at a time, in a hope that we can reduce
	// the amount of seeking needed on rotational media.
	vector<BufferedJPEG> buffered_jpegs;
	size_t frames_written = 0;
	while (num_streams_with_frames_left > 0) {
		// Find the stream with the lowest frame. Lower stream indexes win.
		FrameOnDisk first_frame;
		unsigned first_frame_stream_idx = 0;
		{
			lock_guard<mutex> lock(frame_mu);
			for (size_t stream_idx = 0; stream_idx < MAX_STREAMS; ++stream_idx) {
				if (!has_frames[stream_idx]) {
					continue;
				}
				if (first_frame.pts == -1 || frames[stream_idx][first_frame_idx[stream_idx]].pts < first_frame.pts) {
					first_frame = frames[stream_idx][first_frame_idx[stream_idx]];
					first_frame_stream_idx = stream_idx;
				}
			}
			++first_frame_idx[first_frame_stream_idx];
			if (first_frame_idx[first_frame_stream_idx] >= last_frame_idx[first_frame_stream_idx]) {
				has_frames[first_frame_stream_idx] = false;
				--num_streams_with_frames_left;
			}
		}
		string jpeg = readers[first_frame_stream_idx].read_frame(first_frame);
		int64_t scaled_pts = av_rescale_q(first_frame.pts, AVRational{ 1, TIMEBASE },
		                                  video_streams[first_frame_stream_idx]->time_base);
		buffered_jpegs.emplace_back(BufferedJPEG{ scaled_pts, first_frame_stream_idx, std::move(jpeg) });
		if (buffered_jpegs.size() >= 1000) {
			if (!write_buffered_jpegs(avctx, buffered_jpegs)) {
				QMessageBox msgbox;
				msgbox.setText("Writing frames failed");
				msgbox.exec();
				unlink(filename.c_str());
				return;
			}
			frames_written += buffered_jpegs.size();
			progress.setValue(frames_written);
			buffered_jpegs.clear();
		}
		if (progress.wasCanceled()) {
			unlink(filename.c_str());
			return;
		}
	}

	if (!write_buffered_jpegs(avctx, buffered_jpegs)) {
		QMessageBox msgbox;
		msgbox.setText("Writing frames failed");
		msgbox.exec();
		unlink(filename.c_str());
		return;
	}
	frames_written += buffered_jpegs.size();
	progress.setValue(frames_written);
}

void export_interpolated_clip(const string &filename, const vector<Clip> &clips)
{
	AVFormatContext *avctx = nullptr;
	avformat_alloc_output_context2(&avctx, NULL, NULL, filename.c_str());
	if (avctx == nullptr) {
		QMessageBox msgbox;
		msgbox.setText("Could not allocate FFmpeg context");
		msgbox.exec();
		return;
	}
	AVFormatContextWithCloser closer(avctx);

	int ret = avio_open(&avctx->pb, filename.c_str(), AVIO_FLAG_WRITE);
	if (ret < 0) {
		QMessageBox msgbox;
		msgbox.setText(QString::fromStdString("Could not open output file '" + filename + "'"));
		msgbox.exec();
		return;
	}

	QProgressDialog progress(QString::fromStdString("Exporting to " + filename + "..."), "Abort", 0, 1);
	progress.setWindowTitle("Futatabi");
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(1000);
	progress.setMaximum(100000);
	progress.setValue(0);

	vector<Player::ClipWithRow> clips_with_row;
	for (const Clip &clip : clips) {
		clips_with_row.emplace_back(Player::ClipWithRow{ clip, 0 });
	}
	double total_length = compute_total_time(clips_with_row);

	promise<void> done_promise;
	future<void> done = done_promise.get_future();
	std::atomic<double> current_value{ 0.0 };
	size_t clip_idx = 0;

	Player player(/*destination=*/nullptr, Player::FILE_STREAM_OUTPUT, closer.release());
	player.set_done_callback([&done_promise, &clip_idx, &clips] {
		if (clip_idx >= clips.size()) {
			done_promise.set_value();
		}
	});
	player.set_progress_callback([&current_value, &clips, total_length](const std::map<size_t, double> &player_progress, double time_remaining) {
		current_value = 1.0 - time_remaining / total_length;
	});
	player.play(clips_with_row);
	while (done.wait_for(std::chrono::milliseconds(100)) != future_status::ready && !progress.wasCanceled()) {
		progress.setValue(lrint(100000.0 * current_value));
	}
	if (progress.wasCanceled()) {
		unlink(filename.c_str());
		// Destroying player on scope exit will abort the render job.
	}
}
