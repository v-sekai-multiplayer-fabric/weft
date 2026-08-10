// Load and time the FoundationDB VFS against SQLite on a local file.
//
//   bench_vfs <rows>
//
// It runs the same work twice, once through the default VFS on a local file and once
// through the FoundationDB VFS, and prints operations for each second. The local file
// is the floor, not the target: it is one machine with no durability across machines.
// The number that matters is the ratio, and where the ratio comes from.

#define _POSIX_C_SOURCE 200809L

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int weft_fdb_start(const char *cluster_file);
void weft_fdb_stop(void);
int weft_vfs_register(int make_default);

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int run(sqlite3 *db, const char *sql) {
	char *err = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "  %s -> %s\n", sql, err ? err : "?");
		sqlite3_free(err);
		return 1;
	}
	return 0;
}

struct result {
	double insert_one_by_one;
	double insert_batched;
	double point_reads;
	double scan;
};

static int measure(const char *vfs, const char *name, int rows, struct result *out) {
	sqlite3 *db = NULL;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	if (sqlite3_open_v2(name, &db, flags, vfs)) {
		fprintf(stderr, "open %s: %s\n", name, sqlite3_errmsg(db));
		return 1;
	}
	if (run(db, "PRAGMA journal_mode=MEMORY")) return 1;
	// An actor is the single writer of its own store, so no other connection can change
	// the file. Exclusive locking lets SQLite trust its page cache instead of re-reading
	// page 1 to check the change counter at the start of every read transaction. Over a
	// network database that check is a round trip for each query.
	if (getenv("WEFT_EXCLUSIVE") && run(db, "PRAGMA locking_mode=EXCLUSIVE")) return 1;
	// The cache is the difference between a page read and a network round trip.
	if (getenv("WEFT_CACHE") && run(db, "PRAGMA cache_size=-8000")) return 1;
	if (run(db, "DROP TABLE IF EXISTS kv")) return 1;
	if (run(db, "CREATE TABLE kv (k INTEGER PRIMARY KEY, v TEXT)")) return 1;

	char sql[256];

	// One transaction for each row. This is the actor write path: one put, one commit.
	double t0 = now();
	for (int i = 0; i < rows; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO kv VALUES (%d, 'value-%d')", i, i);
		if (run(db, sql)) return 1;
	}
	out->insert_one_by_one = (double)rows / (now() - t0);

	// One transaction for many rows, which is the same work with the commit cost
	// amortised. The gap between this and the line above is the cost of a commit.
	if (run(db, "DELETE FROM kv")) return 1;
	t0 = now();
	if (run(db, "BEGIN")) return 1;
	for (int i = 0; i < rows; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO kv VALUES (%d, 'value-%d')", i, i);
		if (run(db, sql)) return 1;
	}
	if (run(db, "COMMIT")) return 1;
	out->insert_batched = (double)rows / (now() - t0);

	// Point reads by primary key, which is what an actor does.
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?1", -1, &st, NULL)) return 1;
	t0 = now();
	for (int i = 0; i < rows; i++) {
		sqlite3_reset(st);
		sqlite3_bind_int(st, 1, i);
		if (sqlite3_step(st) != SQLITE_ROW) {
			fprintf(stderr, "missing row %d\n", i);
			return 1;
		}
	}
	out->point_reads = (double)rows / (now() - t0);
	sqlite3_finalize(st);

	// A full scan, which is what read-ahead exists for.
	if (sqlite3_prepare_v2(db, "SELECT count(*), sum(length(v)) FROM kv", -1, &st, NULL))
		return 1;
	t0 = now();
	if (sqlite3_step(st) != SQLITE_ROW) return 1;
	int counted = sqlite3_column_int(st, 0);
	out->scan = (double)rows / (now() - t0);
	sqlite3_finalize(st);

	if (counted != rows) {
		fprintf(stderr, "scan saw %d rows, expected %d\n", counted, rows);
		return 1;
	}

	sqlite3_close(db);
	return 0;
}

int main(int argc, char **argv) {
	int rows = argc > 1 ? atoi(argv[1]) : 1000;

	int err = weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"));
	if (err) {
		fprintf(stderr, "FoundationDB did not start: %d\n", err);
		return 1;
	}
	weft_vfs_register(0);

	struct result local = {0}, fdb = {0};
	printf("rows: %d\n\n", rows);

	if (measure("unix", "/tmp/bench_local.db", rows, &local)) return 1;
	if (measure("weft_fdb", "bench_fdb.db", rows, &fdb)) return 1;

	printf("%-26s %14s %14s %10s\n", "op", "local file/s", "FoundationDB/s", "ratio");
	printf("%-26s %14.0f %14.0f %9.1fx\n", "insert, one commit each",
	       local.insert_one_by_one, fdb.insert_one_by_one,
	       local.insert_one_by_one / fdb.insert_one_by_one);
	printf("%-26s %14.0f %14.0f %9.1fx\n", "insert, one commit for all",
	       local.insert_batched, fdb.insert_batched,
	       local.insert_batched / fdb.insert_batched);
	printf("%-26s %14.0f %14.0f %9.1fx\n", "point read", local.point_reads,
	       fdb.point_reads, local.point_reads / fdb.point_reads);
	printf("%-26s %14.0f %14.0f %9.1fx\n", "scan", local.scan, fdb.scan,
	       local.scan / fdb.scan);

	weft_fdb_stop();
	return 0;
}
