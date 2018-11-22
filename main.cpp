#include <assert.h>
#include <arpa/inet.h>
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
#include <sys/stat.h>
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
#include "frame_on_disk.h"
#include "frame.pb.h"
#include "httpd.h"
#include "mainwindow.h"
#include "player.h"
#include "post_to_main_thread.h"
#include "ref_counted_gl_sync.h"
#include "timebase.h"
#include "ui_mainwindow.h"
#include "vaapi_jpeg_decoder.h"

#include <QApplication>
#include <QGLFormat>
#include <QSurfaceFormat>
#include <movit/init.h>
#include <movit/util.h>

using namespace std;
using namespace std::chrono;

constexpr char frame_magic[] = "Ftbifrm0";
constexpr size_t frame_magic_len = 8;

mutex RefCountedGLsync::fence_lock;
atomic<bool> should_quit{false};

int64_t start_pts = -1;

// TODO: Replace by some sort of GUI control, I guess.
int64_t current_pts = 0;

struct FrameFile {
	FILE *fp = nullptr;
	int filename_idx;
	size_t frames_written_so_far = 0;
};
std::map<int, FrameFile> open_frame_files;

mutex frame_mu;
vector<FrameOnDisk> frames[MAX_STREAMS];  // Under frame_mu.
vector<string> frame_filenames;  // Under frame_mu.

namespace {

FrameOnDisk write_frame(int stream_idx, int64_t pts, const uint8_t *data, size_t size)
{
	if (open_frame_files.count(stream_idx) == 0) {
		char filename[256];
		snprintf(filename, sizeof(filename), "%s/frames/cam%d-pts%09ld.frames",
			global_flags.working_directory.c_str(), stream_idx, pts);
		FILE *fp = fopen(filename, "wb");
		if (fp == nullptr) {
			perror(filename);
			exit(1);
		}

		lock_guard<mutex> lock(frame_mu);
		int filename_idx = frame_filenames.size();
		frame_filenames.push_back(filename);
		open_frame_files[stream_idx] = FrameFile{ fp, filename_idx, 0 };
	}

	FrameFile &file = open_frame_files[stream_idx];
	string filename;
	{
		lock_guard<mutex> lock(frame_mu);
		filename = frame_filenames[file.filename_idx];
	}

	FrameHeaderProto hdr;
	hdr.set_stream_idx(stream_idx);
	hdr.set_pts(pts);
	hdr.set_file_size(size);

	string serialized;
	if (!hdr.SerializeToString(&serialized)) {
		fprintf(stderr, "Frame header serialization failed.\n");
		exit(1);
	}
	uint32_t len = htonl(serialized.size());

	if (fwrite(frame_magic, frame_magic_len, 1, file.fp) != 1) {
		perror("fwrite");
		exit(1);
	}
	if (fwrite(&len, sizeof(len), 1, file.fp) != 1) {
		perror("fwrite");
		exit(1);
	}
	if (fwrite(serialized.data(), serialized.size(), 1, file.fp) != 1) {
		perror("fwrite");
		exit(1);
	}
	off_t offset = ftell(file.fp);
	if (fwrite(data, size, 1, file.fp) != 1) {
		perror("fwrite");
		exit(1);
	}
	fflush(file.fp);  // No fsync(), though. We can accept losing a few frames.
	global_disk_space_estimator->report_write(filename, 8 + sizeof(len) + serialized.size() + size, pts);

	if (++file.frames_written_so_far >= 1000) {
		// Start a new file next time.
		if (fclose(file.fp) != 0) {
			perror("fclose");
			exit(1);
		}
		open_frame_files.erase(stream_idx);

		// TODO: Write to SQLite.
	}

	FrameOnDisk frame;
	frame.pts = pts;
	frame.filename_idx = file.filename_idx;
	frame.offset = offset;
	frame.size = size;

	{
		lock_guard<mutex> lock(frame_mu);
		assert(stream_idx < MAX_STREAMS);
		frames[stream_idx].push_back(frame);
	}

	return frame;
}

} // namespace

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

	string frame_dir = global_flags.working_directory + "/frames";

	struct stat st;
	if (stat(frame_dir.c_str(), &st) == -1) {
		fprintf(stderr, "%s does not exist, creating it.\n", frame_dir.c_str());
		if (mkdir(frame_dir.c_str(), 0777) == -1) {
			perror(global_flags.working_directory.c_str());
			exit(1);
		}
	}

	avformat_network_init();
	global_httpd = new HTTPD;

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

	MainWindow main_window;
	main_window.show();

	global_httpd->add_endpoint("/queue_status", bind(&MainWindow::get_queue_status, &main_window), HTTPD::NO_CORS_POLICY);
	global_httpd->start(global_flags.http_port);

	init_jpeg_vaapi();

	load_existing_frames();
	thread record_thread(record_thread_func);

	int ret = app.exec();

	should_quit = true;
	record_thread.join();
	JPEGFrameView::shutdown();

	return ret;
}

void load_frame_file(const char *filename, unsigned filename_idx)
{
	// TODO: Look up in the SQLite database.

	FILE *fp = fopen(filename, "rb");
	if (fp == nullptr) {
		perror(filename);
		exit(1);
	}

	size_t magic_offset = 0;
	size_t skipped_bytes = 0;
	while (!feof(fp) && !ferror(fp)) {
		int ch = getc(fp);
		if (ch == -1) {
			break;
		}
		if (ch != frame_magic[magic_offset++]) {
			skipped_bytes += magic_offset;
			magic_offset = 0;
			continue;
		}
		if (magic_offset < frame_magic_len) {
			// Still reading the magic (hopefully).
			continue;
		}

		// OK, found the magic. Try to parse the frame header.
		magic_offset = 0;

		if (skipped_bytes > 0)  {
			fprintf(stderr, "WARNING: %s: Skipped %zu garbage bytes in the middle.\n",
				filename, skipped_bytes);
			skipped_bytes = 0;
		}

		uint32_t len;
		if (fread(&len, sizeof(len), 1, fp) != 1) {
			fprintf(stderr, "WARNING: %s: Short read when getting length.\n", filename);
			break;
		}

		string serialized;
		serialized.resize(ntohl(len));
		if (fread(&serialized[0], serialized.size(), 1, fp) != 1) {
			fprintf(stderr, "WARNING: %s: Short read when reading frame header (%zu bytes).\n", filename, serialized.size());
			break;
		}

		FrameHeaderProto hdr;
		if (!hdr.ParseFromString(serialized)) {
			fprintf(stderr, "WARNING: %s: Corrupted frame header.\n", filename);
			continue;
		}

		FrameOnDisk frame;
		frame.pts = hdr.pts();
		frame.offset = ftell(fp);
		frame.filename_idx = filename_idx;
		frame.size = hdr.file_size();

		if (fseek(fp, frame.offset + frame.size, SEEK_SET) == -1) {
			fprintf(stderr, "WARNING: %s: Could not seek past frame (probably truncated).\n", filename);
			continue;
		}

		if (hdr.stream_idx() >= 0 && hdr.stream_idx() < MAX_STREAMS) {
			frames[hdr.stream_idx()].push_back(frame);
			start_pts = max(start_pts, hdr.pts());
		}
	}

	if (skipped_bytes > 0) {
		fprintf(stderr, "WARNING: %s: Skipped %zu garbage bytes at the end.\n",
			filename, skipped_bytes);
	}
}

void load_existing_frames()
{
	string frame_dir = global_flags.working_directory + "/frames";
	DIR *dir = opendir(frame_dir.c_str());
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

		if (de->d_type == DT_REG) {
			string filename = frame_dir + "/" + de->d_name;
			load_frame_file(filename.c_str(), frame_filenames.size());
			frame_filenames.push_back(filename);
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
		sort(frames[stream_idx].begin(), frames[stream_idx].end(),
			[](const auto &a, const auto &b) { return a.pts < b.pts; });
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
		FrameOnDisk frame = write_frame(pkt.stream_index, pts, pkt.data, pkt.size);

		post_to_main_thread([pkt, frame] {
			if (pkt.stream_index == 0) {
				global_mainwindow->ui->input1_display->setFrame(pkt.stream_index, frame);
			} else if (pkt.stream_index == 1) {
				global_mainwindow->ui->input2_display->setFrame(pkt.stream_index, frame);
			} else if (pkt.stream_index == 2) {
				global_mainwindow->ui->input3_display->setFrame(pkt.stream_index, frame);
			} else if (pkt.stream_index == 3) {
				global_mainwindow->ui->input4_display->setFrame(pkt.stream_index, frame);
			}
		});

		if (last_pts != -1 && global_flags.slow_down_input) {
			this_thread::sleep_for(microseconds((pts - last_pts) * 1000000 / TIMEBASE));
		}
		last_pts = pts;
		current_pts = pts;
	}

	return 0;
}

string read_frame(FrameOnDisk frame)
{
	string filename;
	{
		lock_guard<mutex> lock(frame_mu);
		filename = frame_filenames[frame.filename_idx];
	}

	// TODO: cache the open file handles
	FILE *fp = fopen(filename.c_str(), "rb");
	if (fp == nullptr) {
		perror(filename.c_str());
		exit(1);
	}
	if (fseek(fp, frame.offset, SEEK_SET) == -1) {
		perror("fseek");
		exit(1);
	}

	string str;
	str.resize(frame.size);
	if (fread(&str[0], frame.size, 1, fp) != 1) {
		perror("fread");
		exit(1);
	}

	fclose(fp);
	return str;
}
