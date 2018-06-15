#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include <QApplication>

#include "clip_list.h"
#include "defs.h"
#include "mainwindow.h"
#include "ffmpeg_raii.h"
#include "player.h"
#include "post_to_main_thread.h"
#include "ui_mainwindow.h"

using namespace std;
using namespace std::chrono;

// TODO: Replace by some sort of GUI control, I guess.
int64_t current_pts = 0;

string filename_for_frame(unsigned stream_idx, int64_t pts)
{
	char filename[256];
	snprintf(filename, sizeof(filename), "frames/cam%d-pts%09ld.jpeg", stream_idx, pts);
	return filename;
}

mutex frame_mu;
vector<int64_t> frames[MAX_STREAMS];

int record_thread_func();

int main(int argc, char **argv)
{
	av_register_all();
	avformat_network_init();

	QApplication app(argc, argv);
	MainWindow mainWindow;
	mainWindow.show();

	thread(record_thread_func).detach();

	return app.exec();
}

int record_thread_func()
{
	auto format_ctx = avformat_open_input_unique("multiangle.mp4", nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", "example.mp4");
		return 1;
	}

	int64_t last_pts = -1;

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
		//fprintf(stderr, "Got a frame from camera %d, pts = %ld, size = %d\n",
		//	pkt.stream_index, pkt.pts, pkt.size);
		string filename = filename_for_frame(pkt.stream_index, pkt.pts);
		FILE *fp = fopen(filename.c_str(), "wb");
		if (fp == nullptr) {
			perror(filename.c_str());
			exit(1);
		}
		fwrite(pkt.data, pkt.size, 1, fp);
		fclose(fp);

		post_to_main_thread([pkt] {
			if (pkt.stream_index == 0) {
				global_mainwindow->ui->input1_display->setFrame(pkt.stream_index, pkt.pts);
			} else if (pkt.stream_index == 1) {
				global_mainwindow->ui->input2_display->setFrame(pkt.stream_index, pkt.pts);
			} else if (pkt.stream_index == 2) {
				global_mainwindow->ui->input3_display->setFrame(pkt.stream_index, pkt.pts);
			} else if (pkt.stream_index == 3) {
				global_mainwindow->ui->input4_display->setFrame(pkt.stream_index, pkt.pts);
			}
		});

		assert(pkt.stream_index < MAX_STREAMS);
		frames[pkt.stream_index].push_back(pkt.pts);

		// Hack. Assumes a given timebase.
		if (last_pts != -1) {
			this_thread::sleep_for(microseconds((pkt.pts - last_pts) * 1000000 / 12800));
		}
		last_pts = pkt.pts;
		current_pts = pkt.pts;
	}

	return 0;
}
