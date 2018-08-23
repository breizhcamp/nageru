#include <epoxy/gl.h>

#include "gpu_timers.h"

using namespace std;

bool enable_timing = false;
bool detailed_timing = false;
bool in_warmup = false;

pair<GLuint, GLuint> GPUTimers::begin_timer(const string &name, int level)
{
	if (!enable_timing) {
		return make_pair(0, 0);
	}

	GLuint queries[2];
	glGenQueries(2, queries);
	glQueryCounter(queries[0], GL_TIMESTAMP);

	Timer timer;
	timer.name = name;
	timer.level = level;
	timer.query.first = queries[0];
	timer.query.second = queries[1];
	timers.push_back(timer);
	return timer.query;
}

GLint64 find_elapsed(pair<GLuint, GLuint> queries)
{
	// NOTE: This makes the CPU wait for the GPU.
	GLuint64 time_start, time_end;
	glGetQueryObjectui64v(queries.first, GL_QUERY_RESULT, &time_start);
	glGetQueryObjectui64v(queries.second, GL_QUERY_RESULT, &time_end);
	return time_end - time_start;
}

void GPUTimers::print()
{
	for (size_t i = 0; i < timers.size(); ++i) {
		if (timers[i].level >= 4 && !detailed_timing) {
			// In practice, only affects the SOR sub-timers.
			continue;
		}

		GLint64 time_elapsed = find_elapsed(timers[i].query);
		for (int j = 0; j < timers[i].level * 2; ++j) {
			fprintf(stderr, " ");
		}

		if (detailed_timing) {
			// Look for any immediate subtimers, and see if they sum to the large one.
			size_t num_subtimers = 0;
			GLint64 sum_subtimers = 0;
			for (size_t j = i + 1; j < timers.size() && timers[j].level > timers[i].level; ++j) {
				if (timers[j].level != timers[i].level + 1) continue;
				++num_subtimers;
				sum_subtimers += find_elapsed(timers[j].query);
			}

			if (num_subtimers > 0 && (time_elapsed - sum_subtimers) / 1e6 >= 0.01) {
				fprintf(stderr, "%-30s %4.3f ms [%4.3f ms unaccounted for]\n", timers[i].name.c_str(), time_elapsed / 1e6, (time_elapsed - sum_subtimers) / 1e6);
			} else {
				fprintf(stderr, "%-30s %4.3f ms\n", timers[i].name.c_str(), time_elapsed / 1e6);
			}
		} else {
			fprintf(stderr, "%-30s %4.1f ms\n", timers[i].name.c_str(), time_elapsed / 1e6);
		}
	}
}
