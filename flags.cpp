#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

using namespace std;

Flags global_flags;

// Long options that have no corresponding short option.
enum LongOption {
	OPTION_HELP = 1000,
	OPTION_SLOW_DOWN_INPUT = 1001
};

void usage()
{
	fprintf(stderr, "Usage: futatabi [OPTION]... SOURCE_URL\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "      --help                      print usage information\n");
	fprintf(stderr, "      --slow-down-input           slow down input to realtime (default on if no\n");
	fprintf(stderr, "                                    source URL given)");
}

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, OPTION_HELP },
		{ "slow-down-input", no_argument, 0, OPTION_SLOW_DOWN_INPUT },
		{ 0, 0, 0, 0 }
	};
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "q:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case OPTION_SLOW_DOWN_INPUT:
			global_flags.slow_down_input = true;
			break;
		case OPTION_HELP:
			usage();
			exit(0);
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			fprintf(stderr, "\n");
			usage();
			exit(1);
		}
	}
}
