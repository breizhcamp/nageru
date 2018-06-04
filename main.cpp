#include <stdio.h>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

#include "ffmpeg_raii.h"

using namespace std;

int main(void)
{
	av_register_all();
	avformat_network_init();

	auto format_ctx = avformat_open_input_unique("example.mp4", nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", "example.mp4");
		return 1;
	}

	for ( ;; ) {
		AVPacket pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
			&pkt, av_packet_unref);
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		if (av_read_frame(format_ctx.get(), &pkt) != 0) {
			break;
		}
		fprintf(stderr, "Got a frame from camera %d, pts = %ld, size = %d\n",
			pkt.stream_index, pkt.pts, pkt.size);
		char filename[256];
		snprintf(filename, sizeof(filename), "frames/cam%d-pts%09ld.jpeg", pkt.stream_index, pkt.pts);
		FILE *fp = fopen(filename, "wb");
		if (fp == nullptr) {
			perror(filename);
			exit(1);
		}
		fwrite(pkt.data, pkt.size, 1, fp);
		fclose(fp);
	}

	return 0;
}
