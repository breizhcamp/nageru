#ifndef _FRAME_ON_DISK_H
#define _FRAME_ON_DISK_H 1

#include "defs.h"

#include <algorithm>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

extern std::mutex frame_mu;
struct FrameOnDisk {
	int64_t pts = -1;  // -1 means empty.
	off_t offset;
	unsigned filename_idx;
	uint32_t size;  // Not using size_t saves a few bytes; we can have so many frames.
};
extern std::vector<FrameOnDisk> frames[MAX_STREAMS];  // Under frame_mu.
extern std::vector<std::string> frame_filenames;  // Under frame_mu.

static bool inline operator==(const FrameOnDisk &a, const FrameOnDisk &b)
{
	return a.pts == b.pts &&
		a.offset == b.offset &&
		a.filename_idx == b.filename_idx &&
		a.size == b.size;
}

// A helper class to read frames from disk. It caches the file descriptor
// so that the kernel has a better chance of doing readahead when it sees
// the sequential reads. (For this reason, each display has a private
// FrameReader. Thus, we can easily keep multiple open file descriptors around
// for a single .frames file.)
//
// Thread-compatible, but not thread-safe.
class FrameReader {
public:
	FrameReader();
	~FrameReader();
	std::string read_frame(FrameOnDisk frame);

private:
	int fd = -1;
	int last_filename_idx = -1;
};

// Utility functions for dealing with binary search.
inline std::vector<FrameOnDisk>::iterator
find_last_frame_before(std::vector<FrameOnDisk> &frames, int64_t pts_origin)
{
	return std::lower_bound(frames.begin(), frames.end(), pts_origin,
	                        [](const FrameOnDisk &frame, int64_t pts) { return frame.pts < pts; });
}

inline std::vector<FrameOnDisk>::iterator
find_first_frame_at_or_after(std::vector<FrameOnDisk> &frames, int64_t pts_origin)
{
	return std::upper_bound(frames.begin(), frames.end(), pts_origin - 1,
	                        [](int64_t pts, const FrameOnDisk &frame) { return pts < frame.pts; });
}

#endif  // !defined(_FRAME_ON_DISK_H)
