#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include <QApplication>

#include "mainwindow.h"
#include "ffmpeg_raii.h"

#define MAX_STREAMS 16

using namespace std;
using namespace std::chrono;

string filename_for_frame(unsigned stream_idx, int64_t pts)
{
	char filename[256];
	snprintf(filename, sizeof(filename), "frames/cam%d-pts%09ld.jpeg", stream_idx, pts);
	return filename;
}

mutex frame_mu;
vector<int64_t> frames[MAX_STREAMS];

int thread_func();

int main(int argc, char **argv)
{
	av_register_all();
	avformat_network_init();

	QApplication app(argc, argv);
	MainWindow mainWindow;
	mainWindow.show();

	thread(thread_func).detach();

	return app.exec();
}

int thread_func()
{
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
		string filename = filename_for_frame(pkt.stream_index, pkt.pts);
		FILE *fp = fopen(filename.c_str(), "wb");
		if (fp == nullptr) {
			perror(filename.c_str());
			exit(1);
		}
		fwrite(pkt.data, pkt.size, 1, fp);
		fclose(fp);

		assert(pkt.stream_index < MAX_STREAMS);
		frames[pkt.stream_index].push_back(pkt.pts);

		this_thread::sleep_for(milliseconds(1000) / 120);
	}

	return 0;
}
