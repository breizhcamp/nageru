#ifndef _DEFS_H
#define _DEFS_H 1

#define MAX_STREAMS 16
#define CACHE_SIZE 1000  // In number of frames.
#define NUM_CAMERAS 4
#define MUX_BUFFER_SIZE 10485760

#define DEFAULT_STREAM_MUX_NAME "nut"  // Only for HTTP. Local dump guesses from LOCAL_DUMP_SUFFIX.
#define DEFAULT_HTTPD_PORT 9095
#define MUX_OPTS { \
	/* Make seekable .mov files, and keep MP4 muxer from using unlimited amounts of memory. */ \
	{ "movflags", "empty_moov+frag_keyframe+default_base_moof+skip_trailer" }, \
	\
	/* Make for somewhat less bursty stream output when using .mov. */ \
	{ "frag_duration", "125000" }, \
	\
	/* Keep nut muxer from using unlimited amounts of memory. */ \
	{ "write_index", "0" } \
}


#endif  // !defined(_DEFS_H)
