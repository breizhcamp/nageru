#include "shared/disk_space_estimator.h"

#include <memory>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include "shared/metrics.h"
#include "shared/timebase.h"

using namespace std;

DiskSpaceEstimator::DiskSpaceEstimator(DiskSpaceEstimator::callback_t callback)
	: callback(callback)
{
	global_metrics.add("disk_free_bytes", &metric_disk_free_bytes, Metrics::TYPE_GAUGE);
}

void DiskSpaceEstimator::report_write(const string &filename, off_t bytes, uint64_t pts)
{
	total_size += bytes;
	report_write_internal(filename, total_size, pts);
}

void DiskSpaceEstimator::report_append(const string &filename, uint64_t pts)
{
	if (filename != last_filename) {
		last_filename = filename;
		measure_points.clear();
	}

	struct stat st;
	if (stat(filename.c_str(), &st) == -1) {
		perror(filename.c_str());
		return;
	}

	report_write_internal(filename, st.st_size, pts);
}

void DiskSpaceEstimator::report_write_internal(const string &filename, off_t file_size, uint64_t pts)
{
	// Reject points that are out-of-order (happens with B-frames).
	if (!measure_points.empty() && pts <= measure_points.back().pts) {
		return;
	}

	// Remove too old points.
	while (measure_points.size() > 1 && measure_points.front().pts + window_length < pts) {
		measure_points.pop_front();
	}

	struct statfs fst;
	if (statfs(filename.c_str(), &fst) == -1) {
		perror(filename.c_str());
		return;
	}

	off_t free_bytes = off_t(fst.f_bavail) * fst.f_frsize;
	metric_disk_free_bytes = free_bytes;

	if (!measure_points.empty()) {
		double bytes_per_second = double(file_size - measure_points.front().size) /
			(pts - measure_points.front().pts) * TIMEBASE;
		double seconds_left = free_bytes / bytes_per_second;

		// Only report every second, since updating the UI can be expensive.
		if (last_pts_reported == 0 || pts - last_pts_reported >= TIMEBASE) {
			callback(free_bytes, seconds_left);
			last_pts_reported = pts;
		}
	}

	measure_points.push_back({ pts, file_size });
}

DiskSpaceEstimator *global_disk_space_estimator = nullptr;  // Created in MainWindow::MainWindow().
