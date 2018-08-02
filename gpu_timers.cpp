#include <epoxy/gl.h>

#include "gpu_timers.h"

using namespace std;

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

void GPUTimers::print()
{
	for (const Timer &timer : timers) {
		// NOTE: This makes the CPU wait for the GPU.
		GLuint64 time_start, time_end;
		glGetQueryObjectui64v(timer.query.first, GL_QUERY_RESULT, &time_start);
		glGetQueryObjectui64v(timer.query.second, GL_QUERY_RESULT, &time_end);
		//fprintf(stderr, "GPU time used = %.1f ms\n", time_elapsed / 1e6);
		for (int i = 0; i < timer.level * 2; ++i) {
			fprintf(stderr, " ");
		}
		fprintf(stderr, "%-30s %4.1f ms\n", timer.name.c_str(), GLint64(time_end - time_start) / 1e6);
	}
}
