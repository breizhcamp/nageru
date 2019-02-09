#include "frame_on_disk.h"

#include "shared/metrics.h"

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

using namespace std;
using namespace std::chrono;

namespace {

// There can be multiple FrameReader classes, so make all the metrics static.
once_flag frame_metrics_inited;

atomic<int64_t> metric_frame_opened_files{ 0 };
atomic<int64_t> metric_frame_closed_files{ 0 };
atomic<int64_t> metric_frame_read_bytes{ 0 };
atomic<int64_t> metric_frame_read_frames{ 0 };

Summary metric_frame_read_time_seconds;

}  // namespace

FrameReader::FrameReader()
{
	call_once(frame_metrics_inited, [] {
		global_metrics.add("frame_opened_files", &metric_frame_opened_files);
		global_metrics.add("frame_closed_files", &metric_frame_closed_files);
		global_metrics.add("frame_read_bytes", &metric_frame_read_bytes);
		global_metrics.add("frame_read_frames", &metric_frame_read_frames);

		vector<double> quantiles{ 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99 };
		metric_frame_read_time_seconds.init(quantiles, 60.0);
		global_metrics.add("frame_read_time_seconds", &metric_frame_read_time_seconds);
	});
}

FrameReader::~FrameReader()
{
	if (fd != -1) {
		close(fd);
		++metric_frame_closed_files;
	}
}

string FrameReader::read_frame(FrameOnDisk frame)
{
	steady_clock::time_point start = steady_clock::now();

	if (int(frame.filename_idx) != last_filename_idx) {
		if (fd != -1) {
			close(fd);  // Ignore errors.
			++metric_frame_closed_files;
		}

		string filename;
		{
			lock_guard<mutex> lock(frame_mu);
			filename = frame_filenames[frame.filename_idx];
		}

		fd = open(filename.c_str(), O_RDONLY);
		if (fd == -1) {
			perror(filename.c_str());
			exit(1);
		}

		// We want readahead. (Ignore errors.)
		posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

		last_filename_idx = frame.filename_idx;
		++metric_frame_opened_files;
	}

	string str;
	str.resize(frame.size);
	off_t offset = 0;
	while (offset < frame.size) {
		int ret = pread(fd, &str[offset], frame.size - offset, frame.offset + offset);
		if (ret <= 0) {
			perror("pread");
			exit(1);
		}

		offset += ret;
	}

	steady_clock::time_point stop = steady_clock::now();
	metric_frame_read_time_seconds.count_event(duration<double>(stop - start).count());

	metric_frame_read_bytes += frame.size;
	++metric_frame_read_frames;

	return str;
}
