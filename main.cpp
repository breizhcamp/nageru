#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dirent.h>
#include <getopt.h>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "clip_list.h"
#include "context.h"
#include "defs.h"
#include "disk_space_estimator.h"
#include "ffmpeg_raii.h"
#include "flags.h"
#include "httpd.h"
#include "mainwindow.h"
#include "player.h"
#include "post_to_main_thread.h"
#include "ref_counted_gl_sync.h"
#include "timebase.h"
#include "ui_mainwindow.h"
#include "vaapi_jpeg_decoder.h"

#include <QApplication>
#include <movit/init.h>
#include <movit/util.h>

using namespace std;
using namespace std::chrono;

mutex RefCountedGLsync::fence_lock;
atomic<bool> should_quit{false};

int64_t start_pts = -1;

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

void load_existing_frames();
int record_thread_func();

int main(int argc, char **argv)
{
	parse_flags(argc, argv);
	if (optind == argc) {
		global_flags.stream_source = "multiangle.mp4";
		global_flags.slow_down_input = true;
	} else if (optind + 1 == argc) {
		global_flags.stream_source = argv[optind];
	} else {
		usage();
		exit(1);
	}

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

	init_jpeg_vaapi();

	load_existing_frames();
	thread record_thread(record_thread_func);

	int ret = app.exec();

	should_quit = true;
	record_thread.join();
	JPEGFrameView::shutdown();

	return ret;
}

void load_existing_frames()
{
	DIR *dir = opendir("frames/");
	if (dir == nullptr) {
		perror("frames/");
		start_pts = 0;
		return;
	}

	for ( ;; ) {
		errno = 0;
		dirent *de = readdir(dir);
		if (de == nullptr) {
			if (errno != 0) {
				perror("readdir");
				exit(1);
			}
			break;
		}

		int stream_idx;
		int64_t pts;
		if (sscanf(de->d_name, "cam%d-pts%ld.jpeg", &stream_idx, &pts) == 2 &&
		    stream_idx >= 0 && stream_idx < MAX_STREAMS) {
			frames[stream_idx].push_back(pts);
			start_pts = max(start_pts, pts);
		}
	}

	closedir(dir);

	if (start_pts == -1) {
		start_pts = 0;
	} else {
		// Add a gap of one second from the old frames to the new ones.
		start_pts += TIMEBASE;
	}

	for (int stream_idx = 0; stream_idx < MAX_STREAMS; ++stream_idx) {
		sort(frames[stream_idx].begin(), frames[stream_idx].end());
	}
}

int record_thread_func()
{
	auto format_ctx = avformat_open_input_unique(global_flags.stream_source.c_str(), nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", global_flags.stream_source.c_str());
		return 1;
	}

	int64_t last_pts = -1;
	int64_t pts_offset;

	while (!should_quit.load()) {
		AVPacket pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
			&pkt, av_packet_unref);
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;

		// TODO: Make it possible to abort av_read_frame() (use an interrupt callback);
		// right now, should_quit will be ignored if it's hung on I/O.
		if (av_read_frame(format_ctx.get(), &pkt) != 0) {
			break;
		}

		// Convert pts to our own timebase.
		AVRational stream_timebase = format_ctx->streams[pkt.stream_index]->time_base;
		int64_t pts = av_rescale_q(pkt.pts, stream_timebase, AVRational{ 1, TIMEBASE });

		// Translate offset into our stream.
		if (last_pts == -1) {
			pts_offset = start_pts - pts;
		}
		pts = std::max(pts + pts_offset, start_pts);

		//fprintf(stderr, "Got a frame from camera %d, pts = %ld, size = %d\n",
		//	pkt.stream_index, pts, pkt.size);
		string filename = filename_for_frame(pkt.stream_index, pts);
		FILE *fp = fopen(filename.c_str(), "wb");
		if (fp == nullptr) {
			perror(filename.c_str());
			exit(1);
		}
		fwrite(pkt.data, pkt.size, 1, fp);
		fclose(fp);

		global_disk_space_estimator->report_write(filename, pts);

		post_to_main_thread([pkt, pts] {
			if (pkt.stream_index == 0) {
				global_mainwindow->ui->input1_display->setFrame(pkt.stream_index, pts, /*interpolated=*/false);
			} else if (pkt.stream_index == 1) {
				global_mainwindow->ui->input2_display->setFrame(pkt.stream_index, pts, /*interpolated=*/false);
			} else if (pkt.stream_index == 2) {
				global_mainwindow->ui->input3_display->setFrame(pkt.stream_index, pts, /*interpolated=*/false);
			} else if (pkt.stream_index == 3) {
				global_mainwindow->ui->input4_display->setFrame(pkt.stream_index, pts, /*interpolated=*/false);
			}
		});

		assert(pkt.stream_index < MAX_STREAMS);
		frames[pkt.stream_index].push_back(pts);

		if (last_pts != -1 && global_flags.slow_down_input) {
			this_thread::sleep_for(microseconds((pts - last_pts) * 1000000 / TIMEBASE));
		}
		last_pts = pts;
		current_pts = pts;
	}

	return 0;
}
