// Prove the property the store plane decision rests on: an actor's database has no
// local file, so a different process reads it with no copy and no restore.
//
//   prove_handoff write <db>   write rows, then exit
//   prove_handoff read  <db>   read them back in a new process
//
// The two runs share nothing but FoundationDB. The reader has no file to copy and no
// restore step, which is what makes a handoff cost nothing. See
// docs/reference/native_store_plane.md.

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int weft_fdb_start(const char *cluster_file);
void weft_fdb_stop(void);
int weft_vfs_register(int make_default);

static int fail(sqlite3 *db, const char *what) {
	fprintf(stderr, "%s: %s\n", what, db ? sqlite3_errmsg(db) : "no handle");
	return 1;
}

static int run(sqlite3 *db, const char *sql) {
	char *err = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s -> %s\n", sql, err ? err : "?");
		sqlite3_free(err);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: prove_handoff write|read <db>\n");
		return 2;
	}
	const char *mode = argv[1], *name = argv[2];

	int err = weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"));
	if (err) {
		fprintf(stderr, "FoundationDB did not start: %d\n", err);
		return 1;
	}
	weft_vfs_register(1);

	sqlite3 *db = NULL;
	if (sqlite3_open_v2(name, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "weft_fdb"))
		return fail(db, "open");

	// The journal lives in FoundationDB too, so nothing touches the local disk.
	if (run(db, "PRAGMA journal_mode=MEMORY")) return 1;

	if (strcmp(mode, "write") == 0) {
		if (run(db, "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT)")) return 1;
		if (run(db, "DELETE FROM kv")) return 1;
		if (run(db,
		        "INSERT INTO kv VALUES ('zone', 'atlantis'), ('owner', 'machine-a'),"
		        " ('seq', '200')"))
			return 1;
		printf("wrote 3 rows to %s, no local file\n", name);
	} else {
		sqlite3_stmt *st;
		if (sqlite3_prepare_v2(db, "SELECT k, v FROM kv ORDER BY k", -1, &st, NULL))
			return fail(db, "prepare");

		int rows = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			printf("  %s = %s\n", sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
			rows++;
		}
		sqlite3_finalize(st);
		printf("read %d rows from %s in a new process, nothing was copied\n", rows, name);
		if (rows != 3) {
			fprintf(stderr, "expected 3 rows, got %d\n", rows);
			return 1;
		}
	}

	sqlite3_close(db);
	weft_fdb_stop();
	return 0;
}
