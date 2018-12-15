#ifndef _FLAGS_H
#define _FLAGS_H

#include <string>

#include "defs.h"

struct Flags {
	int width = 1280, height = 720;
	std::string stream_source;
	std::string working_directory = ".";
	bool slow_down_input = false;
	int interpolation_quality = 2;  // Can be changed in the menus.
	bool interpolation_quality_set = false;
	uint16_t http_port = DEFAULT_HTTPD_PORT;
	double output_framerate = 60000.0 / 1001.0;
};
extern Flags global_flags;

// The quality setting that VideoStream was initialized to. The quality cannot
// currently be changed, except turning interpolation completely off, so we compare
// against this to give a warning.
extern int flow_initialized_interpolation_quality;

void usage();
void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
