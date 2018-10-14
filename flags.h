#ifndef _FLAGS_H
#define _FLAGS_H

#include <string>

struct Flags {
	std::string stream_source;
	bool slow_down_input = false;
};
extern Flags global_flags;

void usage();
void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
