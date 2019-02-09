#ifndef _BASIC_STATS_H
#define _BASIC_STATS_H

// Holds some metrics for basic statistics about uptime, memory usage and such.

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <memory>

extern bool uses_mlock;

class GPUMemoryStats;

class BasicStats {
public:
	BasicStats(bool verbose, bool use_opengl);
	void update(int frame_num, int stats_dropped_frames);

private:
	std::chrono::steady_clock::time_point start;
	bool verbose;
	std::unique_ptr<GPUMemoryStats> gpu_memory_stats;

	// Metrics.
	std::atomic<int64_t> metric_frames_output_total{0};
	std::atomic<int64_t> metric_frames_output_dropped{0};
	std::atomic<double> metric_start_time_seconds{0.0 / 0.0};
	std::atomic<int64_t> metrics_memory_used_bytes{0};
	std::atomic<double> metrics_memory_locked_limit_bytes{0.0 / 0.0};
};

// Holds some metrics for GPU memory usage. Currently only exposed for NVIDIA cards
// (no-op on all other platforms).

class GPUMemoryStats {
public:
	GPUMemoryStats(bool verbose);
	void update();

private:
	bool verbose, supported;

	// Metrics.
	std::atomic<int64_t> metric_memory_gpu_total_bytes{0};
	std::atomic<int64_t> metric_memory_gpu_dedicated_bytes{0};
	std::atomic<int64_t> metric_memory_gpu_used_bytes{0};
	std::atomic<int64_t> metric_memory_gpu_evicted_bytes{0};
	std::atomic<int64_t> metric_memory_gpu_evictions{0};
};

#endif  // !defined(_BASIC_STATS_H)
