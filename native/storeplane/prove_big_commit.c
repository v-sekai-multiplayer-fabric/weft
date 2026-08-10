// A commit too large for one FoundationDB transaction.
//
//   prove_big_commit <db> <rows> <blob_bytes>
//
// A commit that fits one transaction takes the single transaction path. A larger one
// stages its pages under a txid that no read can reach, and one more transaction then
// moves the head. Every other program here commits a few pages, so this is the only one
// that reaches the staging path.
//
// It found a fault. The page count for one transaction counted the page bytes and not the
// key bytes, so a staged transaction overran the FoundationDB limit of 10 MB and the
// commit failed. A limit derived from the wrong quantity is still a wrong limit.

#define _POSIX_C_SOURCE 200809L
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int weft_fdb_start(const char *cluster_file);
void weft_fdb_stop(void);
int weft_vfs_register(int make_default);

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
	if (argc < 4) {
		fprintf(stderr, "usage: prove_big_commit <db> <rows> <blob_bytes>\n");
		return 2;
	}
	const char *name = argv[1];
	int rows = atoi(argv[2]);
	int blob = atoi(argv[3]);

	if (weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"))) return 1;
	weft_vfs_register(1);

	sqlite3 *db = NULL;
	if (sqlite3_open_v2(name, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "weft_fdb"))
		return 1;
	if (run(db, "PRAGMA journal_mode=MEMORY")) return 1;
	if (run(db, "PRAGMA locking_mode=EXCLUSIVE")) return 1;
	if (run(db, "DROP TABLE IF EXISTS big")) return 1;
	if (run(db, "CREATE TABLE big (k INTEGER PRIMARY KEY, v BLOB)")) return 1;

	unsigned char *payload = malloc((size_t)blob);
	memset(payload, 0xA5, (size_t)blob);

	// One transaction for every row, so the whole thing is one SQLite commit.
	if (run(db, "BEGIN")) return 1;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "INSERT INTO big VALUES (?1, ?2)", -1, &st, NULL)) return 1;
	for (int i = 0; i < rows; i++) {
		sqlite3_reset(st);
		sqlite3_bind_int(st, 1, i);
		sqlite3_bind_blob(st, 2, payload, blob, SQLITE_STATIC);
		if (sqlite3_step(st) != SQLITE_DONE) {
			fprintf(stderr, "insert %d: %s\n", i, sqlite3_errmsg(db));
			return 1;
		}
	}
	sqlite3_finalize(st);
	if (run(db, "COMMIT")) return 1;

	printf("committed %d rows of %d bytes, about %d MiB in one commit\n", rows, blob,
	       rows * blob / (1024 * 1024));

	// Read it back in this process, then check the pages.
	if (sqlite3_prepare_v2(db, "SELECT count(*), sum(length(v)) FROM big", -1, &st, NULL))
		return 1;
	if (sqlite3_step(st) != SQLITE_ROW) return 1;
	int got = sqlite3_column_int(st, 0);
	sqlite3_int64 bytes = sqlite3_column_int64(st, 1);
	sqlite3_finalize(st);
	printf("read back %d rows, %lld bytes\n", got, (long long)bytes);

	int bad = got != rows || bytes != (sqlite3_int64)rows * blob;

	if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL)) return 1;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *line = sqlite3_column_text(st, 0);
		printf("integrity_check: %s\n", line);
		if (!line || sqlite3_stricmp((const char *)line, "ok") != 0) bad = 1;
	}
	sqlite3_finalize(st);

	sqlite3_close(db);
	weft_fdb_stop();
	free(payload);
	return bad;
}
