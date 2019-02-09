#include "db.h"

#include "frame.pb.h"

#include <string>
#include <unordered_set>

using namespace std;

DB::DB(const string &filename)
{
	int ret = sqlite3_open(filename.c_str(), &db);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "%s: %s\n", filename.c_str(), sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_exec(db, R"(
		CREATE TABLE IF NOT EXISTS state (state BLOB);
	)",
	             nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, R"(
		CREATE TABLE IF NOT EXISTS settings (settings BLOB);
	)",
	             nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, R"(
		DROP TABLE file;
	)",
	             nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, R"(
		DROP TABLE frame;
	)",
	             nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, R"(
		CREATE TABLE IF NOT EXISTS filev2 (
			file INTEGER NOT NULL PRIMARY KEY,
			filename VARCHAR NOT NULL UNIQUE,
			size BIGINT NOT NULL,
			frames BLOB NOT NULL
		);
	)",
	             nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);  // Ignore errors.
	sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);  // Ignore errors.
}

StateProto DB::get_state()
{
	StateProto state;

	sqlite3_stmt *stmt;
	int ret = sqlite3_prepare_v2(db, "SELECT state FROM state", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		bool ok = state.ParseFromArray(sqlite3_column_blob(stmt, 0), sqlite3_column_bytes(stmt, 0));
		if (!ok) {
			fprintf(stderr, "State in database is corrupted!\n");
			exit(1);
		}
	} else if (ret != SQLITE_DONE) {
		fprintf(stderr, "SELECT step: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	return state;
}

void DB::store_state(const StateProto &state)
{
	string serialized;
	state.SerializeToString(&serialized);

	int ret = sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "BEGIN: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_exec(db, "DELETE FROM state", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "DELETE: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_stmt *stmt;
	ret = sqlite3_prepare_v2(db, "INSERT INTO state VALUES (?)", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_blob(stmt, 1, serialized.data(), serialized.size(), SQLITE_STATIC);

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		fprintf(stderr, "INSERT step: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "COMMIT: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
}

SettingsProto DB::get_settings()
{
	SettingsProto settings;

	sqlite3_stmt *stmt;
	int ret = sqlite3_prepare_v2(db, "SELECT settings FROM settings", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		bool ok = settings.ParseFromArray(sqlite3_column_blob(stmt, 0), sqlite3_column_bytes(stmt, 0));
		if (!ok) {
			fprintf(stderr, "State in database is corrupted!\n");
			exit(1);
		}
	} else if (ret != SQLITE_DONE) {
		fprintf(stderr, "SELECT step: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	return settings;
}

void DB::store_settings(const SettingsProto &settings)
{
	string serialized;
	settings.SerializeToString(&serialized);

	int ret = sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "BEGIN: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_exec(db, "DELETE FROM settings", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "DELETE: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_stmt *stmt;
	ret = sqlite3_prepare_v2(db, "INSERT INTO settings VALUES (?)", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_blob(stmt, 1, serialized.data(), serialized.size(), SQLITE_STATIC);

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		fprintf(stderr, "INSERT step: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "COMMIT: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
}

vector<DB::FrameOnDiskAndStreamIdx> DB::load_frame_file(const string &filename, size_t size, unsigned filename_idx)
{
	FileContentsProto file_contents;

	sqlite3_stmt *stmt;
	int ret = sqlite3_prepare_v2(db, "SELECT frames FROM filev2 WHERE filename=? AND size=?", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_text(stmt, 1, filename.data(), filename.size(), SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 2, size);

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		bool ok = file_contents.ParseFromArray(sqlite3_column_blob(stmt, 0), sqlite3_column_bytes(stmt, 0));
		if (!ok) {
			fprintf(stderr, "Frame list in database is corrupted!\n");
			exit(1);
		}
	} else if (ret != SQLITE_DONE) {
		fprintf(stderr, "SELECT step: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	vector<FrameOnDiskAndStreamIdx> frames;
	for (const StreamContentsProto &stream : file_contents.stream()) {
		FrameOnDiskAndStreamIdx frame;
		frame.stream_idx = stream.stream_idx();
		for (int i = 0; i < stream.pts_size(); ++i) {
			frame.frame.filename_idx = filename_idx;
			frame.frame.pts = stream.pts(i);
			frame.frame.offset = stream.offset(i);
			frame.frame.size = stream.file_size(i);
			frames.push_back(frame);
		}
	}

	return frames;
}

void DB::store_frame_file(const string &filename, size_t size, const vector<FrameOnDiskAndStreamIdx> &frames)
{
	int ret = sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "BEGIN: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	// Delete any existing instances with this filename.
	sqlite3_stmt *stmt;

	ret = sqlite3_prepare_v2(db, "DELETE FROM filev2 WHERE filename=?", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "DELETE prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_text(stmt, 1, filename.data(), filename.size(), SQLITE_STATIC);

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		fprintf(stderr, "DELETE step: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "DELETE finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	// Create the protobuf blob for the new row.
	FileContentsProto file_contents;
	unordered_set<unsigned> seen_stream_idx;  // Usually only one.
	for (const FrameOnDiskAndStreamIdx &frame : frames) {
		seen_stream_idx.insert(frame.stream_idx);
	}
	for (unsigned stream_idx : seen_stream_idx) {
		StreamContentsProto *stream = file_contents.add_stream();
		stream->set_stream_idx(stream_idx);
		stream->mutable_pts()->Reserve(frames.size());
		stream->mutable_offset()->Reserve(frames.size());
		stream->mutable_file_size()->Reserve(frames.size());
		for (const FrameOnDiskAndStreamIdx &frame : frames) {
			if (frame.stream_idx != stream_idx) {
				continue;
			}
			stream->add_pts(frame.frame.pts);
			stream->add_offset(frame.frame.offset);
			stream->add_file_size(frame.frame.size);
		}
	}
	string serialized;
	file_contents.SerializeToString(&serialized);

	// Insert the new row.
	ret = sqlite3_prepare_v2(db, "INSERT INTO filev2 (filename, size, frames) VALUES (?, ?, ?)", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_text(stmt, 1, filename.data(), filename.size(), SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 2, size);
	sqlite3_bind_blob(stmt, 3, serialized.data(), serialized.size(), SQLITE_STATIC);

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		fprintf(stderr, "INSERT step: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	// Commit.
	ret = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "COMMIT: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
}

void DB::clean_unused_frame_files(const vector<string> &used_filenames)
{
	int ret = sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "BEGIN: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_exec(db, R"(
		CREATE TEMPORARY TABLE used_filenames ( filename VARCHAR NOT NULL PRIMARY KEY )
	)",
	                   nullptr, nullptr, nullptr);

	if (ret != SQLITE_OK) {
		fprintf(stderr, "CREATE TEMPORARY TABLE: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	// Insert the new rows.
	sqlite3_stmt *stmt;
	ret = sqlite3_prepare_v2(db, "INSERT INTO used_filenames (filename) VALUES (?)", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	for (const string &filename : used_filenames) {
		sqlite3_bind_text(stmt, 1, filename.data(), filename.size(), SQLITE_STATIC);

		ret = sqlite3_step(stmt);
		if (ret == SQLITE_ROW) {
			fprintf(stderr, "INSERT step: %s\n", sqlite3_errmsg(db));
			exit(1);
		}

		ret = sqlite3_reset(stmt);
		if (ret == SQLITE_ROW) {
			fprintf(stderr, "INSERT reset: %s\n", sqlite3_errmsg(db));
			exit(1);
		}
	}

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_exec(db, R"(
		DELETE FROM filev2 WHERE filename NOT IN ( SELECT filename FROM used_filenames )
	)",
	                   nullptr, nullptr, nullptr);

	if (ret != SQLITE_OK) {
		fprintf(stderr, "DELETE: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	ret = sqlite3_exec(db, R"(
		DROP TABLE used_filenames
	)",
	                   nullptr, nullptr, nullptr);

	if (ret != SQLITE_OK) {
		fprintf(stderr, "DROP TABLE: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	// Commit.
	ret = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "COMMIT: %s\n", sqlite3_errmsg(db));
		exit(1);
	}
}
