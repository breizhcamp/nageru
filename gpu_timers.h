#ifndef _GPU_TIMERS_H
#define _GPU_TIMERS_H 1

#include <string>
#include <utility>
#include <vector>

#include <epoxy/gl.h>

extern bool enable_timing;
extern bool detailed_timing;

class GPUTimers {
public:
	void print();
	std::pair<GLuint, GLuint> begin_timer(const std::string &name, int level);

private:
	struct Timer {
		std::string name;
		int level;
		std::pair<GLuint, GLuint> query;
	};
	std::vector<Timer> timers;
};

// A simple RAII class for timing until the end of the scope.
class ScopedTimer {
public:
	ScopedTimer(const std::string &name, GPUTimers *timers)
		: timers(timers), level(0)
	{
		query = timers->begin_timer(name, level);
	}

	ScopedTimer(const std::string &name, ScopedTimer *parent_timer)
		: timers(parent_timer->timers),
		  level(parent_timer->level + 1)
	{
		query = timers->begin_timer(name, level);
	}

	~ScopedTimer()
	{
		end();
	}

	void end()
	{
		if (enable_timing && !ended) {
			glQueryCounter(query.second, GL_TIMESTAMP);
			ended = true;
		}
	}

private:
	GPUTimers *timers;
	int level;
	std::pair<GLuint, GLuint> query;
	bool ended = false;
};

#endif  // !defined(_GPU_TIMERS_H)
