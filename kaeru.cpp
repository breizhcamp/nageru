// Kaeru (変える), a simple transcoder intended for use with Nageru.
// This is experimental code, not yet supported.

#include "audio_encoder.h"
#include "defs.h"
#include "flags.h"
#include "ffmpeg_capture.h"
#include "mixer.h"
#include "mux.h"
#include "timebase.h"
#include "x264_encoder.h"

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>

using namespace bmusb;
using namespace movit;
using namespace std;
using namespace std::placeholders;

Mixer *global_mixer = nullptr;

int write_packet(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	static bool seen_sync_markers = false;
	static string stream_mux_header;
	HTTPD *httpd = (HTTPD *)opaque;

	if (type == AVIO_DATA_MARKER_SYNC_POINT || type == AVIO_DATA_MARKER_BOUNDARY_POINT) {
		seen_sync_markers = true;
	} else if (type == AVIO_DATA_MARKER_UNKNOWN && !seen_sync_markers) {
		// We don't know if this is a keyframe or not (the muxer could
		// avoid marking it), so we just have to make the best of it.
		type = AVIO_DATA_MARKER_SYNC_POINT;
	}

	if (type == AVIO_DATA_MARKER_HEADER) {
		stream_mux_header.append((char *)buf, buf_size);
		httpd->set_header(stream_mux_header);
	} else {
		httpd->add_data((char *)buf, buf_size, type == AVIO_DATA_MARKER_SYNC_POINT);
	}
	return buf_size;
}

unique_ptr<Mux> create_mux(HTTPD *httpd, AVOutputFormat *oformat, X264Encoder *x264_encoder, AudioEncoder *audio_encoder)
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = oformat;

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, httpd, nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &write_packet;
	avctx->pb->ignore_boundary_point = 1;

	string video_extradata = x264_encoder->get_global_headers();

	unique_ptr<Mux> mux;
	int time_base = global_flags.stream_coarse_timebase ? COARSE_TIMEBASE : TIMEBASE;
	mux.reset(new Mux(avctx, global_flags.width, global_flags.height, Mux::CODEC_H264, video_extradata, audio_encoder->get_codec_parameters().get(), time_base,
	        /*write_callback=*/nullptr, Mux::WRITE_FOREGROUND, {}));
	return mux;
}

void video_frame_callback(FFmpegCapture *video, X264Encoder *x264_encoder, AudioEncoder *audio_encoder,
                          int64_t video_pts, AVRational video_timebase,
                          int64_t audio_pts, AVRational audio_timebase,
                          uint16_t timecode,
	                  FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
	                  FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)
{
	if (video_pts >= 0 && video_frame.len > 0) {
		video_pts = av_rescale_q(video_pts, video_timebase, AVRational{ 1, TIMEBASE });
		int64_t frame_duration = TIMEBASE * video_format.frame_rate_nom / video_format.frame_rate_den;
		x264_encoder->add_frame(video_pts, frame_duration, video->get_current_frame_ycbcr_format().luma_coefficients, video_frame.data + video_offset, ReceivedTimestamps());
	}
	if (audio_frame.len > 0) {
		// FFmpegCapture takes care of this for us.
		assert(audio_format.num_channels == 2);
		assert(audio_format.sample_rate == OUTPUT_FREQUENCY);

		// TODO: Reduce some duplication against AudioMixer here.
		size_t num_samples = audio_frame.len / (audio_format.bits_per_sample / 8);
		vector<float> float_samples;
		float_samples.resize(num_samples);
		if (audio_format.bits_per_sample == 16) {
			const int16_t *src = (const int16_t *)audio_frame.data;
			float *dst = &float_samples[0];
			for (size_t i = 0; i < num_samples; ++i) {
				*dst++ = le16toh(*src++) * (1.0f / 32768.0f);
			}
		} else if (audio_format.bits_per_sample == 32) {
			const int32_t *src = (const int32_t *)audio_frame.data;
			float *dst = &float_samples[0];
			for (size_t i = 0; i < num_samples; ++i) {
				*dst++ = le32toh(*src++) * (1.0f / 2147483648.0f);
			}
		} else {
			assert(false);
		}
		audio_pts = av_rescale_q(audio_pts, audio_timebase, AVRational{ 1, TIMEBASE });
		audio_encoder->encode_audio(float_samples, audio_pts);
        }

	if (video_frame.owner) {
		video_frame.owner->release_frame(video_frame);
	}
	if (audio_frame.owner) {
		audio_frame.owner->release_frame(audio_frame);
	}
}

void audio_frame_callback(Mux *mux, const AVPacket *pkt, AVRational timebase)
{
	mux->add_packet(*pkt, pkt->pts, pkt->dts == AV_NOPTS_VALUE ? pkt->pts : pkt->dts, timebase);
}

int main(int argc, char *argv[])
{
	parse_flags(PROGRAM_KAERU, argc, argv);
	if (optind + 1 != argc) {
		usage(PROGRAM_KAERU);
		exit(1);
	}

	av_register_all();
	avformat_network_init();

	HTTPD httpd;

	AVOutputFormat *oformat = av_guess_format(global_flags.stream_mux_name.c_str(), nullptr, nullptr);
	assert(oformat != nullptr);

	unique_ptr<AudioEncoder> audio_encoder;
	if (global_flags.transcode_audio) {
		if (global_flags.stream_audio_codec_name.empty()) {
			audio_encoder.reset(new AudioEncoder(AUDIO_OUTPUT_CODEC_NAME, DEFAULT_AUDIO_OUTPUT_BIT_RATE, oformat));
		} else {
			audio_encoder.reset(new AudioEncoder(global_flags.stream_audio_codec_name, global_flags.stream_audio_codec_bitrate, oformat));
		}
	}

	X264Encoder x264_encoder(oformat);
	unique_ptr<Mux> http_mux = create_mux(&httpd, oformat, &x264_encoder, audio_encoder.get());
	if (global_flags.transcode_audio) {
		audio_encoder->add_mux(http_mux.get());
	}
	x264_encoder.add_mux(http_mux.get());

	FFmpegCapture video(argv[optind], global_flags.width, global_flags.height);
	video.set_pixel_format(FFmpegCapture::PixelFormat_NV12);
	video.set_frame_callback(bind(video_frame_callback, &video, &x264_encoder, audio_encoder.get(), _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11));
	if (!global_flags.transcode_audio) {
		video.set_audio_callback(bind(audio_frame_callback, http_mux.get(), _1, _2));
	}
	video.configure_card();
	video.start_bm_capture();
	video.change_rate(2.0);  // Be sure never to really fall behind, but also don't dump huge amounts of stuff onto x264.

	httpd.start(9095);

	for ( ;; ) {
		sleep(3600);
	}
}
