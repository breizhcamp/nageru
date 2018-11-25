#ifndef DB_H
#define DB_H 1

#include "state.pb.h"

#include <sqlite3.h>
#include <string>
#include <vector>

#include "frame_on_disk.h"

class DB {
public:
	explicit DB(const std::string &filename);
	DB(const DB &) = delete;

	StateProto get_state();
	void store_state(const StateProto &state);

	struct FrameOnDiskAndStreamIdx {
		FrameOnDisk frame;
		unsigned stream_idx;
	};
	std::vector<FrameOnDiskAndStreamIdx> load_frame_file(const std::string &filename, size_t size, unsigned frame_idx);  // Empty = none found, or there were no frames.
	void store_frame_file(const std::string &filename, size_t size, const std::vector<FrameOnDiskAndStreamIdx> &frames);

private:
	StateProto state;
	sqlite3 *db;
};

#endif  // !defined(DB_H)
