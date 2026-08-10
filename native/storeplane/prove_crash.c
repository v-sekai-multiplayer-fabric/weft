// Crash a writer during a commit, and look for a database that holds half of one.
//
//   prove_crash <db> <rows> kill <delay_ms>   crash at a wall clock delay
//   prove_crash <db> <rows> at <n>            crash before the Nth store commit
//
// `PRAGMA integrity_check` cannot see this fault. A database that mixes pages from two
// commits is structurally valid, so the check returns ok and the contents are still
// wrong. This program looks for the fault directly.
//
// Every round writes the same text into every row, so a database that finished a commit
// holds one distinct value. Two distinct values mean a commit was torn: some pages come
// from the new round and some come from the old one.
//
// The two modes answer different questions. `kill` sends SIGKILL after a delay, which is
// the crash a machine failure gives, but the instant it lands is not repeatable. `at`
// stops the writer before a numbered commit, which covers the same states and repeats
// exactly. A search over crash points needs the second mode, because a search cannot
// address a failure it cannot repeat.
//
// The crash points are the states FoundationDB can hold. The writer commits a sequence
// of transactions, so stopping before the Nth leaves the first N-1 of them. Walking N
// over its range walks every state a crash can leave behind.
//
// The child does the writing and the parent crashes it. The parent starts the
// FoundationDB client after the fork, so no client state crosses it.
//
// Losing the round that was in flight is correct and expected. `Weft.Actor.Store` accepts
// that a crash loses the last few commits. A torn database is a different failure,
// because it loses the whole actor rather than the last write.
//
// Exit status: 0 clean, 1 torn or a failed check, 2 the crash point was never reached.

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

static sqlite3 *open_db(const char *name) {
	sqlite3 *db = NULL;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	if (sqlite3_open_v2(name, &db, flags, "weft_fdb")) {
		fprintf(stderr, "open: %s\n", db ? sqlite3_errmsg(db) : "no handle");
		return NULL;
	}
	// The journal stays in memory, so the database file is the only thing on the VFS.
	// A commit must therefore be atomic in the VFS itself, which is what this tests.
	if (run(db, "PRAGMA journal_mode=MEMORY")) return NULL;
	if (run(db, "PRAGMA locking_mode=EXCLUSIVE")) return NULL;
	return db;
}

// Write rounds until the crash arrives. Each round rewrites every row in one
// transaction, so a round either lands whole or does not land.
//
// `rounds` of zero means write until something else stops this process, which is what
// the kill mode wants. A crash point instead stops the writer from the inside, so that
// mode bounds the rounds and reports when the point was out of range.
static int writer(const char *name, int rows, int rounds) {
	if (weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"))) return 1;
	weft_vfs_register(1);

	sqlite3 *db = open_db(name);
	if (!db) return 1;

	if (run(db, "DROP TABLE IF EXISTS kv")) return 1;
	if (run(db, "CREATE TABLE kv (k INTEGER PRIMARY KEY, v TEXT)")) return 1;

	char sql[128];
	if (run(db, "BEGIN")) return 1;
	for (int i = 0; i < rows; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO kv VALUES (%d, 'round-0')", i);
		if (run(db, sql)) return 1;
	}
	if (run(db, "COMMIT")) return 1;

	for (int round = 1; rounds == 0 || round <= rounds; round++) {
		snprintf(sql, sizeof sql, "UPDATE kv SET v = 'round-%d'", round);
		if (run(db, sql)) return 1;
	}
	// Every round landed, so the crash point is past the end of the sequence.
	return 0;
}

// Read the database the dead writer left behind.
static int verify(const char *name) {
	if (weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"))) return 1;
	weft_vfs_register(1);

	sqlite3 *db = open_db(name);
	if (!db) return 1;

	int bad = 0;

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL)) {
		fprintf(stderr, "prepare integrity_check: %s\n", sqlite3_errmsg(db));
		return 1;
	}
	while (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *line = sqlite3_column_text(st, 0);
		printf("integrity_check: %s\n", line ? (const char *)line : "?");
		if (!line || sqlite3_stricmp((const char *)line, "ok") != 0) bad = 1;
	}
	sqlite3_finalize(st);

	// The test. One distinct value means the last commit landed whole or did not land.
	// Two mean the database holds pages from two rounds at once.
	if (sqlite3_prepare_v2(db, "SELECT count(*), count(DISTINCT v), min(v), max(v) FROM kv",
	                       -1, &st, NULL)) {
		// No table means the writer died before it seeded. Nothing was torn.
		printf("rows: 0, distinct: 0, seed did not finish\n");
		sqlite3_close(db);
		weft_fdb_stop();
		return bad;
	}
	if (sqlite3_step(st) == SQLITE_ROW) {
		int rows = sqlite3_column_int(st, 0);
		int distinct = sqlite3_column_int(st, 1);
		const unsigned char *lo = sqlite3_column_text(st, 2);
		const unsigned char *hi = sqlite3_column_text(st, 3);
		printf("rows: %d, distinct: %d, value: %s\n", rows, distinct,
		       lo ? (const char *)lo : "none");
		if (distinct > 1) {
			printf("TORN: %s and %s in one database\n", (const char *)lo, (const char *)hi);
			bad = 1;
		}
	}
	sqlite3_finalize(st);

	sqlite3_close(db);
	weft_fdb_stop();
	return bad;
}

// How many rounds the writer runs to reach crash point N.
//
// Each round commits at least once, so N rounds always reach the Nth commit. The bound
// comes from the crash point and is not a number somebody picked, so every crash point
// is reachable and the search never has to guess how long to wait.
static int rounds_for(long crash_point) { return (int)crash_point; }

int main(int argc, char **argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: prove_crash <db> <rows> kill <delay_ms>\n");
		fprintf(stderr, "       prove_crash <db> <rows> at <n>\n");
		return 2;
	}
	const char *name = argv[1];
	int rows = atoi(argv[2]);
	const char *mode = argv[3];
	long amount = atol(argv[4]);

	int at_mode = strcmp(mode, "at") == 0;
	if (!at_mode && strcmp(mode, "kill") != 0) {
		fprintf(stderr, "mode must be kill or at\n");
		return 2;
	}

	// Fork before the FoundationDB client starts, so the child gets no client state.
	pid_t child = fork();
	if (child < 0) {
		perror("fork");
		return 1;
	}
	if (child == 0) {
		if (at_mode) {
			// The VFS reads this when it starts, and stops the process before the
			// numbered commit.
			char at[32];
			snprintf(at, sizeof at, "%ld", amount);
			setenv("WEFT_CRASH_AT_COMMIT", at, 1);
			_exit(writer(name, rows, rounds_for(amount)));
		}
		_exit(writer(name, rows, 0));
	}

	if (!at_mode) {
		struct timespec wait = {amount / 1000, (amount % 1000) * 1000000L};
		nanosleep(&wait, NULL);
		// SIGKILL, so the writer gets no chance to finish or to clean up. This is the
		// crash a machine failure gives.
		kill(child, SIGKILL);
	}

	int status = 0;
	waitpid(child, &status, 0);

	if (at_mode && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		// The writer finished every round, so this crash point is past the end.
		printf("unreached: fewer than %ld commits in %d rounds\n", amount, rounds_for(amount));
		return 2;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
		fprintf(stderr, "writer failed before the crash\n");
		return 1;
	}

	return verify(name);
}
