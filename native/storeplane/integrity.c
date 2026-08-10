// Ask SQLite to check a database that lives in FoundationDB.
//
//   integrity <db>
//
// `PRAGMA integrity_check` is SQLite's own audit of its B-tree: page linkage, cell
// order, index against table, and free list. It reads every page, so it walks the VFS
// over the whole database. It prints "ok" or a list of the faults it found.
//
// This is the reusable suite. It is stronger than a test we would write, because it
// checks invariants of the file format rather than of our expectations.

#define _POSIX_C_SOURCE 200809L

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>

int weft_fdb_start(const char *cluster_file);
void weft_fdb_stop(void);
int weft_vfs_register(int make_default);

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: integrity <db>\n");
		return 2;
	}

	if (weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"))) {
		fprintf(stderr, "FoundationDB did not start\n");
		return 1;
	}
	weft_vfs_register(0);

	sqlite3 *db = NULL;
	if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, "weft_fdb")) {
		fprintf(stderr, "open: %s\n", sqlite3_errmsg(db));
		return 1;
	}

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL)) {
		fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
		return 1;
	}

	int bad = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *line = sqlite3_column_text(st, 0);
		printf("%s\n", line);
		if (line && sqlite3_stricmp((const char *)line, "ok") != 0) bad = 1;
	}

	sqlite3_finalize(st);
	sqlite3_close(db);
	weft_fdb_stop();
	return bad;
}
