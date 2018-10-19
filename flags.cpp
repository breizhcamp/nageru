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
	fprintf(stderr, "  -q, --interpolation-quality N   1 = fastest\n");
	fprintf(stderr, "                                  2 = default (realtime 720p on fast embedded GPUs)\n");
	fprintf(stderr, "                                  3 = good (realtime 720p on GTX 970 or so)\n");
	fprintf(stderr, "                                  4 = best (not realtime on any current GPU)\n");
	fprintf(stderr, "  -d, --working-directory DIR     where to store frames and database\n");
}

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, OPTION_HELP },
		{ "slow-down-input", no_argument, 0, OPTION_SLOW_DOWN_INPUT },
		{ "interpolation-quality", required_argument, 0, 'q' },
		{ "working-directory", required_argument, 0, 'd' },
		{ 0, 0, 0, 0 }
	};
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "q:d:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case OPTION_SLOW_DOWN_INPUT:
			global_flags.slow_down_input = true;
			break;
		case 'q':
			global_flags.interpolation_quality = atoi(optarg);
			break;
		case 'd':
			global_flags.working_directory = optarg;
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

	if (global_flags.interpolation_quality < 1 || global_flags.interpolation_quality > 4) {
		fprintf(stderr, "Interpolation quality must be 1, 2, 3 or 4.\n");
		usage();
		exit(1);
	}
}
