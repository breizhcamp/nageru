#include "video_stream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include "httpd.h"
#include "jpeg_frame_view.h"
#include "mux.h"
#include "player.h"

using namespace std;

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

void VideoStream::start()
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = av_guess_format("nut", nullptr, nullptr);

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &VideoStream::write_packet2_thunk;
	avctx->pb->ignore_boundary_point = 1;

	Mux::Codec video_codec = Mux::CODEC_MJPEG;

	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	string video_extradata;

	constexpr int width = 1280, height = 720;  // Doesn't matter for MJPEG.
	stream_mux.reset(new Mux(avctx, width, height, video_codec, video_extradata, /*audio_codec_parameters=*/nullptr, COARSE_TIMEBASE,
		/*write_callback=*/nullptr, Mux::WRITE_FOREGROUND, {}));


	encode_thread = thread(&VideoStream::encode_thread_func, this);
}

void VideoStream::stop()
{
	encode_thread.join();
}

void VideoStream::schedule_original_frame(int64_t output_pts, unsigned stream_idx, int64_t input_pts)
{
	unique_lock<mutex> lock(queue_lock);
	QueuedFrame qf;
	qf.output_pts = output_pts;
	qf.stream_idx = stream_idx;
	qf.input_first_pts = input_pts;	
	frame_queue.push_back(qf);
	queue_nonempty.notify_all();
}

void VideoStream::encode_thread_func()
{
	for ( ;; ) {
		QueuedFrame qf;
		{
			unique_lock<mutex> lock(queue_lock);
			queue_nonempty.wait(lock, [this]{
				return !frame_queue.empty();
			});
			qf = frame_queue.front();
			frame_queue.pop_front();
		}

		if (qf.type == QueuedFrame::ORIGINAL) {
			string jpeg = read_file(filename_for_frame(qf.stream_idx, qf.input_first_pts));
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)jpeg.data();
			pkt.size = jpeg.size();
			stream_mux->add_packet(pkt, qf.output_pts, qf.output_pts);
		}		
	}
}

int VideoStream::write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	VideoStream *video_stream = (VideoStream *)opaque;
	return video_stream->write_packet2(buf, buf_size, type, time);
}

int VideoStream::write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
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

