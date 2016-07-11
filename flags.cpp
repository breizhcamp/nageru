#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

using namespace std;

Flags global_flags;

void usage()
{
	fprintf(stderr, "Usage: nageru [OPTION]...\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h, --help                      print usage information\n");
	fprintf(stderr, "  -c, --num-cards                 set number of input cards, including fake cards (default 2)\n");
	fprintf(stderr, "  -C, --num-fake-cards            set number of fake cards (default 0)\n");
	fprintf(stderr, "  -t, --theme=FILE                choose theme (default theme.lua)\n");
	fprintf(stderr, "  -v, --va-display=SPEC           VA-API device for H.264 encoding\n");
	fprintf(stderr, "                                    ($DISPLAY spec or /dev/dri/render* path)\n");
	fprintf(stderr, "  -m, --map-signal=SIGNAL,CARD    set a default card mapping (can be given multiple times)\n");
	fprintf(stderr, "      --http-uncompressed-video   send uncompressed NV12 video to HTTP clients\n");
	fprintf(stderr, "      --http-x264-video           send x264-compressed video to HTTP clients\n");
	fprintf(stderr, "      --x264-preset               x264 quality preset (default " X264_DEFAULT_PRESET ")\n");
	fprintf(stderr, "      --x264-tune                 x264 tuning (default " X264_DEFAULT_TUNE ", can be blank)\n");
	fprintf(stderr, "      --x264-speedcontrol         try to match x264 preset to available CPU speed\n");
	fprintf(stderr, "      --x264-speedcontrol-verbose  output speedcontrol debugging statistics\n");
	fprintf(stderr, "      --x264-bitrate              x264 bitrate (in kilobit/sec, default %d)\n",
		DEFAULT_X264_OUTPUT_BIT_RATE);
	fprintf(stderr, "      --x264-vbv-bufsize          x264 VBV size (in kilobits, 0 = one-frame VBV,\n");
	fprintf(stderr, "                                  default: same as --x264-bitrate, that is, one-second VBV)\n");
	fprintf(stderr, "      --x264-vbv-max-bitrate      x264 local max bitrate (in kilobit/sec per --vbv-bufsize,\n");
	fprintf(stderr, "                                  0 = no limit, default: same as --x264-bitrate, i.e., CBR)\n");
	fprintf(stderr, "      --x264-param=NAME[,VALUE]   set any x264 parameter, for fine tuning\n");
	fprintf(stderr, "      --http-mux=NAME             mux to use for HTTP streams (default " DEFAULT_STREAM_MUX_NAME ")\n");
	fprintf(stderr, "      --http-audio-codec=NAME     audio codec to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is to use the same as for the recording)\n");
	fprintf(stderr, "      --http-audio-bitrate=KBITS  audio codec bit rate to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is %d, ignored unless --http-audio-codec is set)\n",
		DEFAULT_AUDIO_OUTPUT_BIT_RATE / 1000);
	fprintf(stderr, "      --http-coarse-timebase      use less timebase for HTTP (recommended for muxers\n");
	fprintf(stderr, "                                  that handle large pts poorly, like e.g. MP4)\n");
	fprintf(stderr, "      --flat-audio                start with most audio processing turned off\n");
	fprintf(stderr, "      --disable-alsa-output       disable audio monitoring via ALSA\n");
	fprintf(stderr, "      --no-flush-pbos             do not explicitly signal texture data uploads\n");
	fprintf(stderr, "                                    (will give display corruption, but makes it\n");
	fprintf(stderr, "                                    possible to run with apitrace in real time)\n");
}

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "num-cards", required_argument, 0, 'c' },
		{ "num-fake-cards", required_argument, 0, 'C' },
		{ "theme", required_argument, 0, 't' },
		{ "map-signal", required_argument, 0, 'm' },
		{ "va-display", required_argument, 0, 1000 },
		{ "http-uncompressed-video", no_argument, 0, 1001 },
		{ "http-x264-video", no_argument, 0, 1008 },
		{ "x264-preset", required_argument, 0, 1009 },
		{ "x264-tune", required_argument, 0, 1010 },
		{ "x264-speedcontrol", no_argument, 0, 1015 },
		{ "x264-speedcontrol-verbose", no_argument, 0, 1016 },
		{ "x264-bitrate", required_argument, 0, 1011 },
		{ "x264-vbv-bufsize", required_argument, 0, 1012 },
		{ "x264-vbv-max-bitrate", required_argument, 0, 1013 },
		{ "x264-param", required_argument, 0, 1017 },
		{ "http-mux", required_argument, 0, 1004 },
		{ "http-coarse-timebase", no_argument, 0, 1005 },
		{ "http-audio-codec", required_argument, 0, 1006 },
		{ "http-audio-bitrate", required_argument, 0, 1007 },
		{ "flat-audio", no_argument, 0, 1002 },
		{ "disable-alsa-output", no_argument, 0, 1014 },
		{ "no-flush-pbos", no_argument, 0, 1003 },
		{ 0, 0, 0, 0 }
	};
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "c:C:t:v:m:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':
			global_flags.num_cards = atoi(optarg);
			break;
		case 'C':
			global_flags.num_fake_cards = atoi(optarg);
			break;
		case 't':
			global_flags.theme_filename = optarg;
			break;
		case 'm': {
			char *ptr = strchr(optarg, ',');
			if (ptr == nullptr) {
				fprintf(stderr, "ERROR: Invalid argument '%s' to --map-signal (needs a signal and a card number, separated by comma)\n", optarg);
				exit(1);
			}
			*ptr = '\0';
			const int signal_num = atoi(optarg);
			const int card_num = atoi(ptr + 1);
			if (global_flags.default_stream_mapping.count(signal_num)) {
				fprintf(stderr, "ERROR: Signal %d already mapped to card %d\n",
					signal_num, global_flags.default_stream_mapping[signal_num]);
				exit(1);
			}
			global_flags.default_stream_mapping[signal_num] = card_num;
			break;
		}
		case 1000:
			global_flags.va_display = optarg;
			break;
		case 1001:
			global_flags.uncompressed_video_to_http = true;
			break;
		case 1004:
			global_flags.stream_mux_name = optarg;
			break;
		case 1005:
			global_flags.stream_coarse_timebase = true;
			break;
		case 1006:
			global_flags.stream_audio_codec_name = optarg;
			break;
		case 1007:
			global_flags.stream_audio_codec_bitrate = atoi(optarg) * 1000;
			break;
		case 1008:
			global_flags.x264_video_to_http = true;
			break;
		case 1009:
			global_flags.x264_preset = optarg;
			break;
		case 1010:
			global_flags.x264_tune = optarg;
			break;
		case 1015:
			global_flags.x264_speedcontrol = true;
			break;
		case 1016:
			global_flags.x264_speedcontrol_verbose = true;
			break;
		case 1011:
			global_flags.x264_bitrate = atoi(optarg);
			break;
		case 1012:
			global_flags.x264_vbv_buffer_size = atoi(optarg);
			break;
		case 1013:
			global_flags.x264_vbv_max_bitrate = atoi(optarg);
			break;
		case 1017:
			global_flags.x264_extra_param.push_back(optarg);
			break;
		case 1002:
			global_flags.flat_audio = true;
			break;
		case 1014:
			global_flags.enable_alsa_output = false;
			break;
		case 1003:
			global_flags.flush_pbos = false;
			break;
		case 'h':
			usage();
			exit(0);
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			fprintf(stderr, "\n");
			usage();
			exit(1);
		}
	}

	if (global_flags.uncompressed_video_to_http &&
	    global_flags.x264_video_to_http) {
		fprintf(stderr, "ERROR: --http-uncompressed-video and --http-x264-video are mutually incompatible\n");
		exit(1);
	}
	if (global_flags.num_fake_cards > global_flags.num_cards) {
		fprintf(stderr, "ERROR: More fake cards then total cards makes no sense\n");
		exit(1);
	}
	if (global_flags.num_cards <= 0) {
		fprintf(stderr, "ERROR: --num-cards must be at least 1\n");
		exit(1);
	}
	if (global_flags.num_fake_cards < 0) {
		fprintf(stderr, "ERROR: --num-fake-cards cannot be negative\n");
		exit(1);
	}
	if (global_flags.x264_speedcontrol) {
		if (!global_flags.x264_preset.empty() && global_flags.x264_preset != "faster") {
			fprintf(stderr, "WARNING: --x264-preset is overridden by --x264-speedcontrol (implicitly uses \"faster\" as base preset)\n");
		}
		global_flags.x264_preset = "faster";
	} else if (global_flags.x264_preset.empty()) {
		global_flags.x264_preset = X264_DEFAULT_PRESET;
	}

	for (pair<int, int> mapping : global_flags.default_stream_mapping) {
		if (mapping.second >= global_flags.num_cards) {
			fprintf(stderr, "ERROR: Signal %d mapped to card %d, which doesn't exist (try adjusting --num-cards)\n",
				mapping.first, mapping.second);
			exit(1);
		}
	}
}
