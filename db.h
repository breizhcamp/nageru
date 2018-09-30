#ifndef DB_H
#define DB_H 1

#include "state.pb.h"
#include <sqlite3.h>

class DB {
public:
	explicit DB(const char *filename);
	DB(const DB &) = delete;

	StateProto get_state();
	void store_state(const StateProto &state);

private:
	StateProto state;
	sqlite3 *db;
};

#endif  // !defined(DB_H)
