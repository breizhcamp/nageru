#ifndef _SHARED_DEFS_H
#define _SHARED_DEFS_H 1

// This flag is only supported in FFmpeg 3.3 and up, and we only require 3.1.
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 71, 100)
#define MUX_SKIP_TRAILER "+skip_trailer"
#else
#define MUX_SKIP_TRAILER ""
#endif

#define MUX_OPTS { \
	/* Make seekable .mov files, and keep MP4 muxer from using unlimited amounts of memory. */ \
	{ "movflags", "empty_moov+frag_keyframe+default_base_moof" MUX_SKIP_TRAILER }, \
	\
	/* Make for somewhat less bursty stream output when using .mov. */ \
	{ "frag_duration", "125000" }, \
	\
	/* Keep nut muxer from using unlimited amounts of memory. */ \
	{ "write_index", "0" } \
}

// In bytes. Beware, if too small, stream clients will start dropping data.
// For mov, you want this at 10MB or so (for the reason mentioned above),
// but for nut, there's no flushing, so such a large mux buffer would cause
// the output to be very uneven.
#define MUX_BUFFER_SIZE 10485760

#endif  // !defined(_SHARED_DEFS_H)
