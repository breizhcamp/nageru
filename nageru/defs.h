#ifndef _DEFS_H
#define _DEFS_H

#include <libavformat/version.h>

#define OUTPUT_FREQUENCY 48000  // Currently needs to be exactly 48000, since bmusb outputs in that.
#define MAX_FPS 60
#define FAKE_FPS 25  // Must be an integer.
#define MAX_VIDEO_CARDS 16
#define MAX_ALSA_CARDS 16
#define MAX_BUSES 256  // Audio buses.

// For deinterlacing. See also comments on InputState.
#define FRAME_HISTORY_LENGTH 5

#define AUDIO_OUTPUT_CODEC_NAME "pcm_s32le"
#define DEFAULT_AUDIO_OUTPUT_BIT_RATE 0
#define DEFAULT_X264_OUTPUT_BIT_RATE 4500  // 5 Mbit after making room for some audio and TCP overhead.

#define LOCAL_DUMP_PREFIX "record-"
#define LOCAL_DUMP_SUFFIX ".nut"
#define DEFAULT_STREAM_MUX_NAME "nut"  // Only for HTTP. Local dump guesses from LOCAL_DUMP_SUFFIX.
#define DEFAULT_HTTPD_PORT 9095

#include "shared/shared_defs.h"

// In number of frames. Comes in addition to any internal queues in x264
// (frame threading, lookahead, etc.).
#define X264_QUEUE_LENGTH 50

#define X264_DEFAULT_PRESET "ultrafast"
#define X264_DEFAULT_TUNE "film"

#endif  // !defined(_DEFS_H)
