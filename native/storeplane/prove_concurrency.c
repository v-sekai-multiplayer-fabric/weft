// What two writers do to one database.
//
//   prove_concurrency <db> <id> <rows>
//
// The VFS locks nothing. Every xLock returns SQLITE_OK, so two writers both believe
// they hold the write lock. weft's single-writer invariant comes from the actor owning
// its id, not from the store.
//
// That invariant is not absolute. `Weft.Actors` says Horde is CRDT-based and chooses
// availability, so during a partition each side may briefly run its own instance. This
// program runs that case on purpose, so the result is measured rather than assumed.
//
// rivet handles it with a fence. `depot_client_types::is_head_fence_mismatch` rejects a
// writer whose head token is stale. This VFS has no fence yet.

#define _POSIX_C_SOURCE 200809L

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int weft_fdb_start(const char *cluster_file);
void weft_fdb_stop(void);
int weft_vfs_register(int make_default);

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: prove_concurrency <db> <id> <rows>\n");
		return 2;
	}
	const char *name = argv[1];
	int id = atoi(argv[2]);
	int rows = atoi(argv[3]);

	if (weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"))) return 1;
	weft_vfs_register(0);

	sqlite3 *db = NULL;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	if (sqlite3_open_v2(name, &db, flags, "weft_fdb")) {
		fprintf(stderr, "writer %d open: %s\n", id, sqlite3_errmsg(db));
		return 1;
	}
	sqlite3_exec(db, "PRAGMA journal_mode=MEMORY", NULL, NULL, NULL);
	sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS kv (k INTEGER PRIMARY KEY, w INT)", NULL,
	             NULL, NULL);

	int wrote = 0, refused = 0;
	char sql[160];
	for (int i = 0; i < rows; i++) {
		snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO kv VALUES (%d, %d)", i, id);
		char *err = NULL;
		if (sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK) {
			wrote++;
		} else {
			refused++;
			sqlite3_free(err);
		}
	}

	printf("writer %d: wrote %d, refused %d\n", id, wrote, refused);
	sqlite3_close(db);
	weft_fdb_stop();
	return 0;
}
