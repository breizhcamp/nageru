#include "disk_space_estimator.h"

#include "timebase.h"

#include <memory>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>

DiskSpaceEstimator::DiskSpaceEstimator(DiskSpaceEstimator::callback_t callback)
	: callback(callback)
{
}

void DiskSpaceEstimator::report_write(const std::string &filename, uint64_t pts)
{
	// Reject points that are out-of-order (happens with B-frames).
	if (!measure_points.empty() && pts <= measure_points.back().pts) {
		return;
	}

	// Remove too old points.
	while (measure_points.size() > 1 && measure_points.front().pts + window_length < pts) {
		measure_points.pop_front();
	}

	struct stat st;
	if (stat(filename.c_str(), &st) == -1) {
		perror(filename.c_str());
		return;
	}

	total_size += st.st_size;

	struct statfs fst;
	if (statfs(filename.c_str(), &fst) == -1) {
		perror(filename.c_str());
		return;
	}

	off_t free_bytes = off_t(fst.f_bavail) * fst.f_frsize;

	if (!measure_points.empty()) {
		double bytes_per_second = double(total_size - measure_points.front().size) /
			(pts - measure_points.front().pts) * TIMEBASE;
		double seconds_left = free_bytes / bytes_per_second;

		// Only report every second, since updating the UI can be expensive.
		if (last_pts_reported == 0 || pts - last_pts_reported >= TIMEBASE) {
			callback(free_bytes, seconds_left);
			last_pts_reported = pts;
		}
	}

	measure_points.push_back({ pts, total_size });
}

DiskSpaceEstimator *global_disk_space_estimator = nullptr;  // Created in MainWindow::MainWindow().
