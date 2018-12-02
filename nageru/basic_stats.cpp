#include "basic_stats.h"
#include "shared/metrics.h"

#include <assert.h>
#include <sys/resource.h>
#include <epoxy/gl.h>

// Epoxy seems to be missing these. Taken from the NVX_gpu_memory_info spec.
#ifndef GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
#define GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX          0x9047
#endif
#ifndef GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
#define GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX    0x9048
#endif
#ifndef GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
#define GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX  0x9049
#endif
#ifndef GPU_MEMORY_INFO_EVICTION_COUNT_NVX
#define GPU_MEMORY_INFO_EVICTION_COUNT_NVX            0x904A
#endif
#ifndef GPU_MEMORY_INFO_EVICTED_MEMORY_NVX
#define GPU_MEMORY_INFO_EVICTED_MEMORY_NVX            0x904B
#endif

using namespace std;
using namespace std::chrono;

bool uses_mlock = false;

BasicStats::BasicStats(bool verbose, bool use_opengl)
	: verbose(verbose)
{
	start = steady_clock::now();

	metric_start_time_seconds = get_timestamp_for_metrics();
	global_metrics.add("frames_output_total", &metric_frames_output_total);
	global_metrics.add("frames_output_dropped", &metric_frames_output_dropped);
	global_metrics.add("start_time_seconds", &metric_start_time_seconds, Metrics::TYPE_GAUGE);
	global_metrics.add("memory_used_bytes", &metrics_memory_used_bytes);
	global_metrics.add("memory_locked_limit_bytes", &metrics_memory_locked_limit_bytes);

	// TODO: It would be nice to compile this out entirely for Kaeru,
	// to avoid pulling in the symbols from libGL/Epoxy.
	if (use_opengl) {
		gpu_memory_stats.reset(new GPUMemoryStats(verbose));
	}
}

void BasicStats::update(int frame_num, int stats_dropped_frames)
{
	steady_clock::time_point now = steady_clock::now();
	double elapsed = duration<double>(now - start).count();

	metric_frames_output_total = frame_num;
	metric_frames_output_dropped = stats_dropped_frames;

	if (frame_num % 100 != 0) {
		return;
	}

	if (verbose) {
		printf("%d frames (%d dropped) in %.3f seconds = %.1f fps (%.1f ms/frame)",
			frame_num, stats_dropped_frames, elapsed, frame_num / elapsed,
			1e3 * elapsed / frame_num);
	}

	// Check our memory usage, to see if we are close to our mlockall()
	// limit (if at all set).
	rusage used;
	if (getrusage(RUSAGE_SELF, &used) == -1) {
		perror("getrusage(RUSAGE_SELF)");
		assert(false);
	}
	metrics_memory_used_bytes = used.ru_maxrss * 1024;

	if (uses_mlock) {
		rlimit limit;
		if (getrlimit(RLIMIT_MEMLOCK, &limit) == -1) {
			perror("getrlimit(RLIMIT_MEMLOCK)");
			assert(false);
		}
		metrics_memory_locked_limit_bytes = limit.rlim_cur;

		if (verbose) {
			if (limit.rlim_cur == 0) {
				printf(", using %ld MB memory (locked)",
						long(used.ru_maxrss / 1024));
			} else {
				printf(", using %ld / %ld MB lockable memory (%.1f%%)",
						long(used.ru_maxrss / 1024),
						long(limit.rlim_cur / 1048576),
						float(100.0 * (used.ru_maxrss * 1024.0) / limit.rlim_cur));
			}
		}
	} else {
		metrics_memory_locked_limit_bytes = 0.0 / 0.0;
		if (verbose) {
			printf(", using %ld MB memory (not locked)",
					long(used.ru_maxrss / 1024));
		}
	}

	if (gpu_memory_stats != nullptr) {
		gpu_memory_stats->update();
	}

	if (verbose) {
		printf("\n");
	}
}

GPUMemoryStats::GPUMemoryStats(bool verbose)
	: verbose(verbose)
{
	// GL_NV_query_memory is exposed but supposedly only works on
	// Quadro/Titan cards, so we use GL_NVX_gpu_memory_info even though it's
	// formally marked as experimental.
	// Intel/Mesa doesn't seem to have anything comparable (at least nothing
	// that gets the amount of _available_ memory).
	supported = epoxy_has_gl_extension("GL_NVX_gpu_memory_info");
	if (supported) {
		global_metrics.add("memory_gpu_total_bytes", &metric_memory_gpu_total_bytes, Metrics::TYPE_GAUGE);
		global_metrics.add("memory_gpu_dedicated_bytes", &metric_memory_gpu_dedicated_bytes, Metrics::TYPE_GAUGE);
		global_metrics.add("memory_gpu_used_bytes", &metric_memory_gpu_used_bytes, Metrics::TYPE_GAUGE);
		global_metrics.add("memory_gpu_evicted_bytes", &metric_memory_gpu_evicted_bytes, Metrics::TYPE_GAUGE);
		global_metrics.add("memory_gpu_evictions", &metric_memory_gpu_evictions);
	}
}

void GPUMemoryStats::update()
{
	if (!supported) {
		return;
	}

	GLint total, dedicated, available, evicted, evictions;
	glGetIntegerv(GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
	glGetIntegerv(GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &dedicated);
	glGetIntegerv(GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &available);
	glGetIntegerv(GPU_MEMORY_INFO_EVICTED_MEMORY_NVX, &evicted);
	glGetIntegerv(GPU_MEMORY_INFO_EVICTION_COUNT_NVX, &evictions);

	if (glGetError() == 0) {
		metric_memory_gpu_total_bytes = int64_t(total) * 1024;
		metric_memory_gpu_dedicated_bytes = int64_t(dedicated) * 1024;
		metric_memory_gpu_used_bytes = int64_t(total - available) * 1024;
		metric_memory_gpu_evicted_bytes = int64_t(evicted) * 1024;
		metric_memory_gpu_evictions = evictions;

		if (verbose) {
			printf(", using %d / %d MB GPU memory (%.1f%%)",
				(total - available) / 1024, total / 1024,
				float(100.0 * (total - available) / total));
		}
	}
}
