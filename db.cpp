#include "db.h"

#include <string>

using namespace std;

DB::DB(const char *filename)
{
	int ret = sqlite3_open(filename, &db);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "%s: %s\n", filename, sqlite3_errmsg(db));
		exit(1);
	}

	sqlite3_exec(db, R"(
		CREATE TABLE IF NOT EXISTS state (state BLOB);
	)", nullptr, nullptr, nullptr);  // Ignore errors.
}

StateProto DB::get_state()
{
	StateProto state;

	sqlite3_stmt *stmt;
	int ret = sqlite3_prepare(db, "SELECT state FROM state", -1, &stmt, 0);
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
	ret = sqlite3_prepare(db, "INSERT INTO state VALUES (?)", -1, &stmt, 0);
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
