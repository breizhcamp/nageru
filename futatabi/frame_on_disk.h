#ifndef _FRAME_ON_DISK_H
#define _FRAME_ON_DISK_H 1

#include <mutex>
#include <string>
#include <vector>

#include <stdint.h>

#include "defs.h"

extern std::mutex frame_mu;
struct FrameOnDisk {
        int64_t pts = -1;  // -1 means empty.
        off_t offset;
        unsigned filename_idx;
        uint32_t size;  // Not using size_t saves a few bytes; we can have so many frames.
};
extern std::vector<FrameOnDisk> frames[MAX_STREAMS];  // Under frame_mu.
extern std::vector<std::string> frame_filenames;  // Under frame_mu.

// A helper class to read frames from disk. It caches the file descriptor
// so that the kernel has a better chance of doing readahead when it sees
// the sequential reads. (For this reason, each display has a private
// FrameReader. Thus, we can easily keep multiple open file descriptors around
// for a single .frames file.)
class FrameReader {
public:
	~FrameReader();
	std::string read_frame(FrameOnDisk frame);

private:
	int fd = -1;
	int last_filename_idx = -1;
};

#endif  // !defined(_FRAME_ON_DISK_H)
