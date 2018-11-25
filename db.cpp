#include "db.h"

#include <string>

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
	)", nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, R"(
		CREATE TABLE IF NOT EXISTS file (
			file INTEGER NOT NULL PRIMARY KEY,
			filename VARCHAR NOT NULL UNIQUE,
			size BIGINT NOT NULL
		);
	)", nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, R"(
		CREATE TABLE IF NOT EXISTS frame (
			file INTEGER NOT NULL REFERENCES file ON DELETE CASCADE,
			stream_idx INTEGER NOT NULL,
			pts BIGINT NOT NULL,
			offset BIGINT NOT NULL,
			size INTEGER NOT NULL
		);
	)", nullptr, nullptr, nullptr);  // Ignore errors.

	sqlite3_exec(db, "CREATE INDEX frame_file ON FRAME ( file );", nullptr, nullptr, nullptr);  // Ignore errors.

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

vector<DB::FrameOnDiskAndStreamIdx> DB::load_frame_file(const string &filename, size_t size, unsigned filename_idx)
{
	sqlite3_stmt *stmt;
	int ret = sqlite3_prepare_v2(db, "SELECT pts, offset, frame.size, stream_idx FROM file JOIN frame USING (file) WHERE filename=? AND file.size=?", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_text(stmt, 1, filename.data(), filename.size(), SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 2, size);

	vector<FrameOnDiskAndStreamIdx> frames;
	do {
		ret = sqlite3_step(stmt);
		if (ret == SQLITE_ROW) {
			FrameOnDiskAndStreamIdx frame;
			frame.frame.filename_idx = filename_idx;
			frame.frame.pts = sqlite3_column_int64(stmt, 0);
			frame.frame.offset = sqlite3_column_int64(stmt, 1);
			frame.frame.size = sqlite3_column_int(stmt, 2);
			frame.stream_idx = sqlite3_column_int(stmt, 3);
			frames.push_back(frame);
		} else if (ret != SQLITE_DONE) {
			fprintf(stderr, "SELECT step: %s\n", sqlite3_errmsg(db));
			exit(1);
		}
	} while (ret != SQLITE_DONE);

	ret = sqlite3_finalize(stmt);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "SELECT finalize: %s\n", sqlite3_errmsg(db));
		exit(1);
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

	// Delete any existing instances with this filename. This also includes
	// deleting any associated frames, due to the ON CASCADE DELETE constraint.
	sqlite3_stmt *stmt;
	ret = sqlite3_prepare_v2(db, "DELETE FROM file WHERE filename=?", -1, &stmt, 0);
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

	// Insert the new row.
	ret = sqlite3_prepare_v2(db, "INSERT INTO file (filename, size) VALUES (?, ?)", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_text(stmt, 1, filename.data(), filename.size(), SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 2, size);

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

	// Insert the actual frames.
	int64_t rowid = sqlite3_last_insert_rowid(db);

	ret = sqlite3_prepare_v2(db, "INSERT INTO frame (file, stream_idx, pts, offset, size) VALUES (?, ?, ?, ?, ?)", -1, &stmt, 0);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "INSERT prepare: %s\n", sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_bind_int64(stmt, 1, rowid);

	for (const FrameOnDiskAndStreamIdx &frame : frames) {
		sqlite3_bind_int64(stmt, 2, frame.stream_idx);
		sqlite3_bind_int64(stmt, 3, frame.frame.pts);
		sqlite3_bind_int64(stmt, 4, frame.frame.offset);
		sqlite3_bind_int(stmt, 5, frame.frame.size);

		ret = sqlite3_step(stmt);
		if (ret == SQLITE_ROW) {
			fprintf(stderr, "INSERT step: %s\n", sqlite3_errmsg(db));
			exit(1);
		}

		ret = sqlite3_reset(stmt);
		if (ret != SQLITE_OK) {
			fprintf(stderr, "INSERT reset: %s\n", sqlite3_errmsg(db));
			exit(1);
		}
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
