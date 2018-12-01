#include <fcntl.h>
#include <unistd.h>

#include "frame_on_disk.h"

using namespace std;

FrameReader::~FrameReader()
{
	if (fd != -1) {
		close(fd);
	}
}

string FrameReader::read_frame(FrameOnDisk frame)
{
	if (int(frame.filename_idx) != last_filename_idx) {
		if (fd != -1) {
			close(fd);  // Ignore errors.
		}

		string filename;
		{
			lock_guard<mutex> lock(frame_mu);
			filename = frame_filenames[frame.filename_idx];
		}

		fd = open(filename.c_str(), O_RDONLY);
		if (fd == -1) {
			perror(filename.c_str());
			exit(1);
		}

		// We want readahead. (Ignore errors.)
		posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

		last_filename_idx = frame.filename_idx;
	}

	string str;
	str.resize(frame.size);
	off_t offset = 0;
	while (offset < frame.size) {
		int ret = pread(fd, &str[offset], frame.size - offset, frame.offset + offset);
		if (ret <= 0) {
			perror("pread");
			exit(1);
		}

		offset += ret;
	}
	return str;
}
