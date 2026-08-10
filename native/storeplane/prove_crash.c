// Kill a writer in the middle of a commit, then ask SQLite whether the database
// survived.
//
//   prove_crash <db> <rows> [kill_delay_ms]
//
// The writer forks. The child writes rows until the parent kills it with SIGKILL, so
// the process dies with a commit in flight. The parent then runs
// `PRAGMA integrity_check` on what is left.
//
// This is the case a store must survive, and the case the current VFS is least likely
// to survive. `journal_mode=MEMORY` keeps the rollback journal in memory, so a process
// that dies mid-commit leaves no journal to roll back from. On a local file SQLite
// would recover from the journal on the next open.
//
// The fix is not a journal. It is the rivet layout in ../spec/Store.lean, where a
// commit is one FoundationDB transaction and a half-written commit cannot be seen.

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int weft_fdb_start(const char *cluster_file);
void weft_fdb_stop(void);
int weft_vfs_register(int make_default);

static int open_db(const char *name, sqlite3 **db, int flags) {
	if (sqlite3_open_v2(name, db, flags, "weft_fdb")) {
		fprintf(stderr, "open: %s\n", sqlite3_errmsg(*db));
		return 1;
	}
	sqlite3_exec(*db, "PRAGMA journal_mode=MEMORY", NULL, NULL, NULL);
	return 0;
}

static void write_forever(const char *name, int rows) {
	if (weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"))) _exit(1);
	weft_vfs_register(0);

	sqlite3 *db = NULL;
	if (open_db(name, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) _exit(1);
	sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS kv (k INTEGER PRIMARY KEY, v TEXT)", NULL,
	             NULL, NULL);

	char sql[256];
	for (int round = 0;; round++) {
		// A wide transaction, so a kill is likely to land inside one.
		sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
		for (int i = 0; i < rows; i++) {
			snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO kv VALUES (%d, 'round-%d')", i,
			         round);
			sqlite3_exec(db, sql, NULL, NULL, NULL);
		}
		sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
	}
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: prove_crash <db> <rows> [kill_delay_ms]\n");
		return 2;
	}
	const char *name = argv[1];
	int rows = atoi(argv[2]);
	// The commit window is short, so a fixed delay rarely lands inside it. The soak
	// varies this to hunt for the window rather than to hope.
	int delay_ms = argc > 3 ? atoi(argv[3]) : 700;

	pid_t child = fork();
	if (child == 0) {
		write_forever(name, rows);
		_exit(0);
	}

	// Let the writer get into a commit, then kill it without warning.
	usleep((useconds_t)delay_ms * 1000);
	kill(child, SIGKILL);
	int status;
	waitpid(child, &status, 0);
	printf("writer killed mid-commit (signal %d)\n", WTERMSIG(status));

	// A different process now opens what is left.
	if (weft_fdb_start(getenv("WEFT_FDB_CLUSTER_FILE"))) return 1;
	weft_vfs_register(0);

	sqlite3 *db = NULL;
	if (open_db(name, &db, SQLITE_OPEN_READONLY)) return 1;

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL)) {
		fprintf(stderr, "the database cannot even be read: %s\n", sqlite3_errmsg(db));
		return 1;
	}

	int bad = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *line = sqlite3_column_text(st, 0);
		printf("integrity_check: %s\n", line);
		if (line && sqlite3_stricmp((const char *)line, "ok") != 0) bad = 1;
	}
	sqlite3_finalize(st);

	// A torn commit shows as rows from two rounds in one table.
	if (!sqlite3_prepare_v2(db, "SELECT count(DISTINCT v) FROM kv", -1, &st, NULL) &&
	    sqlite3_step(st) == SQLITE_ROW) {
		int rounds = sqlite3_column_int(st, 0);
		printf("distinct round values in the table: %d\n", rounds);
		if (rounds > 1) {
			printf("TORN: one commit was applied in part\n");
			bad = 1;
		}
		sqlite3_finalize(st);
	}

	sqlite3_close(db);
	weft_fdb_stop();
	return bad;
}
