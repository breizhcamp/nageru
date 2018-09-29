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

#include <movit/init.h>
#include <movit/util.h>

#include "clip_list.h"
#include "context.h"
#include "defs.h"
#include "mainwindow.h"
#include "ffmpeg_raii.h"
#include "httpd.h"
#include "player.h"
#include "post_to_main_thread.h"
#include "ref_counted_gl_sync.h"
#include "timebase.h"
#include "ui_mainwindow.h"
#include "vaapi_jpeg_decoder.h"

using namespace std;
using namespace std::chrono;

std::mutex RefCountedGLsync::fence_lock;

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
HTTPD *global_httpd;

int record_thread_func();

int main(int argc, char **argv)
{
	avformat_network_init();
	global_httpd = new HTTPD;
	global_httpd->start(DEFAULT_HTTPD_PORT);

	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);

	QSurfaceFormat fmt;
	fmt.setDepthBufferSize(0);
	fmt.setStencilBufferSize(0);
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setMajorVersion(4);
	fmt.setMinorVersion(5);

	// Turn off vsync, since Qt generally gives us at most frame rate
	// (display frequency) / (number of QGLWidgets active).
	fmt.setSwapInterval(0);

	QSurfaceFormat::setDefaultFormat(fmt);

	QGLFormat::setDefaultFormat(QGLFormat::fromSurfaceFormat(fmt));

	QApplication app(argc, argv);
	global_share_widget = new QGLWidget();
	if (!global_share_widget->isValid()) {
		fprintf(stderr, "Failed to initialize OpenGL. Futatabi needs at least OpenGL 4.5 to function properly.\n");
		exit(1);
	}

	// Initialize Movit.
	{
		QSurface *surface = create_surface();
		QOpenGLContext *context = create_context(surface);
		make_current(context, surface);
		CHECK(movit::init_movit(MOVIT_SHADER_DIR, movit::MOVIT_DEBUG_OFF));
		delete_context(context);
		// TODO: Delete the surface, too.
	}

	MainWindow mainWindow;
	mainWindow.show();

	thread(record_thread_func).detach();

	init_jpeg_vaapi();

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

		// Convert pts to our own timebase.
		// TODO: Figure out offsets, too.
		AVRational stream_timebase = format_ctx->streams[pkt.stream_index]->time_base;
		pkt.pts = av_rescale_q(pkt.pts, stream_timebase, AVRational{ 1, TIMEBASE });

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
				global_mainwindow->ui->input1_display->setFrame(pkt.stream_index, pkt.pts, /*interpolated=*/false);
			} else if (pkt.stream_index == 1) {
				global_mainwindow->ui->input2_display->setFrame(pkt.stream_index, pkt.pts, /*interpolated=*/false);
			} else if (pkt.stream_index == 2) {
				global_mainwindow->ui->input3_display->setFrame(pkt.stream_index, pkt.pts, /*interpolated=*/false);
			} else if (pkt.stream_index == 3) {
				global_mainwindow->ui->input4_display->setFrame(pkt.stream_index, pkt.pts, /*interpolated=*/false);
			}
		});

		assert(pkt.stream_index < MAX_STREAMS);
		frames[pkt.stream_index].push_back(pkt.pts);

		// Hack. Remove when we're dealing with live streams.
		if (last_pts != -1) {
			this_thread::sleep_for(microseconds((pkt.pts - last_pts) * 1000000 / TIMEBASE));
		}
		last_pts = pkt.pts;
		current_pts = pkt.pts;
	}

	return 0;
}
