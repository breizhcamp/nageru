#ifndef _FLAGS_H
#define _FLAGS_H

#include <string>

#include "defs.h"

struct Flags {
	std::string stream_source;
	std::string working_directory = ".";
	bool slow_down_input = false;
	int interpolation_quality = 2;
	uint16_t http_port = DEFAULT_HTTPD_PORT;
};
extern Flags global_flags;

void usage();
void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
