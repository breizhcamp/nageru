#ifndef _FLAGS_H
#define _FLAGS_H

#include <string>

struct Flags {
	std::string stream_source;
	std::string working_directory = ".";
	bool slow_down_input = false;
	int interpolation_quality = 2;
};
extern Flags global_flags;

void usage();
void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
