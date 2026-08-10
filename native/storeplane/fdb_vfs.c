// A SQLite VFS whose files live in FoundationDB.
//
// This is the first increment of the store plane described in
// docs/reference/native_store_plane.md. It proves the property the whole decision
// rests on: there is no local file, so an actor's database moves between machines
// with no copy and no restore step.
//
// Layout. A file is a sequence of fixed-size blocks:
//
//   ("weft", "afile", name, block_index) -> block bytes
//   ("weft", "asize", name)              -> file size, 8 bytes big endian
//
// One key for one block is the simplest layout that is correct. rivet's layout adds
// PIDX, DELTA by txid, and SHARD by as_of_txid, so that a commit is one transaction
// and compaction never rewrites a live read. docs/spec/Store.lean models that layout
// and proves the rule compaction must obey. This file does not implement it yet, and
// the next increment replaces the block layout with it.
//
// A block is 4096 bytes, which is SQLite's page size, and it is far below
// FoundationDB's 100 kB value limit.
//
// Locking is a no-op. An actor is the single writer of its own store, so no lease and
// no lock are needed. See the Weft moduledoc.

#include <fdb_c.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLOCK 4096
#define MAX_NAME 512

static FDBDatabase *g_db;
static pthread_t g_network_thread;

// ── FoundationDB helpers ──────────────────────────────────────────────────────

static void *run_network(void *unused) {
	(void)unused;
	fdb_run_network();
	return NULL;
}

// Start the client once for the process. `cluster_file` may be NULL for the default.
int weft_fdb_start(const char *cluster_file) {
	fdb_error_t err = fdb_select_api_version(FDB_API_VERSION);
	if (err) return err;
	if ((err = fdb_setup_network())) return err;
	if (pthread_create(&g_network_thread, NULL, run_network, NULL)) return -1;
	return fdb_create_database(cluster_file, &g_db);
}

void weft_fdb_stop(void) {
	if (g_db) fdb_database_destroy(g_db);
	fdb_stop_network();
	pthread_join(g_network_thread, NULL);
}

// Block the calling thread until a future is ready. The store plane runs its own
// threads, so blocking here does not touch a BEAM scheduler.
static fdb_error_t await(FDBFuture *f) {
	fdb_error_t err = fdb_future_block_until_ready(f);
	if (!err) err = fdb_future_get_error(f);
	return err;
}

// Key: "weft/afile/<name>/<block>" and "weft/asize/<name>". A block index is written
// big endian so that a range read gives the blocks in order.
static int block_key(uint8_t *out, const char *name, uint32_t index) {
	int n = snprintf((char *)out, MAX_NAME, "weft/afile/%s/", name);
	for (int i = 0; i < 4; i++) out[n + i] = (uint8_t)(index >> (24 - 8 * i));
	return n + 4;
}

static int size_key(uint8_t *out, const char *name) {
	return snprintf((char *)out, MAX_NAME, "weft/asize/%s", name);
}

// The fence. Opening a database takes ownership by raising this number. A writer that
// holds an older number is no longer the owner, and its writes are refused.
//
// Without it two writers both succeed and one set of writes disappears with no error,
// because the locks below are no-ops. weft's single-writer invariant comes from the
// actor owning its id, and `Weft.Actors` says Horde chooses availability, so during a
// partition each side may briefly run its own instance. The fence turns that from
// silent loss into a loud failure. rivet does the same, in
// `depot_client_types::is_head_fence_mismatch`.
static int fence_key(uint8_t *out, const char *name) {
	return snprintf((char *)out, MAX_NAME, "weft/afence/%s", name);
}

static int64_t read_u64(FDBTransaction *tr, const uint8_t *key, int klen) {
	FDBFuture *f = fdb_transaction_get(tr, key, klen, 0);
	int64_t out = 0;
	if (!await(f)) {
		fdb_bool_t present;
		const uint8_t *val;
		int vlen;
		if (!fdb_future_get_value(f, &present, &val, &vlen) && present && vlen == 8)
			for (int i = 0; i < 8; i++) out = (out << 8) | val[i];
	}
	fdb_future_destroy(f);
	return out;
}

static void write_u64(FDBTransaction *tr, const uint8_t *key, int klen, int64_t v) {
	uint8_t val[8];
	for (int i = 0; i < 8; i++) val[i] = (uint8_t)(v >> (56 - 8 * i));
	fdb_transaction_set(tr, key, klen, val, 8);
}

// Raise the fence and return the new value, which the caller then holds.
static int64_t claim_fence(const char *name) {
	uint8_t key[MAX_NAME];
	int klen = fence_key(key, name);
	FDBTransaction *tr;
	if (fdb_database_create_transaction(g_db, &tr)) return -1;

	int64_t next = read_u64(tr, key, klen) + 1;
	write_u64(tr, key, klen, next);

	FDBFuture *commit = fdb_transaction_commit(tr);
	fdb_error_t err = await(commit);
	fdb_future_destroy(commit);
	fdb_transaction_destroy(tr);
	return err ? -1 : next;
}

static int64_t read_size(const char *name) {
	uint8_t key[MAX_NAME];
	int klen = size_key(key, name);
	FDBTransaction *tr;
	if (fdb_database_create_transaction(g_db, &tr)) return -1;

	FDBFuture *f = fdb_transaction_get(tr, key, klen, 0);
	int64_t size = 0;
	if (!await(f)) {
		fdb_bool_t present;
		const uint8_t *val;
		int vlen;
		if (!fdb_future_get_value(f, &present, &val, &vlen) && present && vlen == 8) {
			for (int i = 0; i < 8; i++) size = (size << 8) | val[i];
		}
	}
	fdb_future_destroy(f);
	fdb_transaction_destroy(tr);
	return size;
}

// ── The file ──────────────────────────────────────────────────────────────────

typedef struct {
	sqlite3_file base;
	char name[MAX_NAME];
	int64_t fence;
} FdbFile;

static int fdb_read(sqlite3_file *file, void *buf, int amt, sqlite3_int64 off) {
	FdbFile *f = (FdbFile *)file;
	uint8_t *out = buf;
	memset(out, 0, (size_t)amt);

	FDBTransaction *tr;
	if (fdb_database_create_transaction(g_db, &tr)) return SQLITE_IOERR_READ;

	int short_read = 0;
	for (int done = 0; done < amt;) {
		sqlite3_int64 at = off + done;
		uint32_t index = (uint32_t)(at / BLOCK);
		int within = (int)(at % BLOCK);
		int want = amt - done;
		if (want > BLOCK - within) want = BLOCK - within;

		uint8_t key[MAX_NAME];
		int klen = block_key(key, f->name, index);
		FDBFuture *fu = fdb_transaction_get(tr, key, klen, 0);

		if (await(fu)) {
			fdb_future_destroy(fu);
			fdb_transaction_destroy(tr);
			return SQLITE_IOERR_READ;
		}

		fdb_bool_t present;
		const uint8_t *val;
		int vlen;
		if (!fdb_future_get_value(fu, &present, &val, &vlen) && present) {
			int have = vlen - within;
			if (have > 0) memcpy(out + done, val + within, have < want ? (size_t)have : (size_t)want);
			if (have < want) short_read = 1;
		} else {
			short_read = 1;
		}
		fdb_future_destroy(fu);
		done += want;
	}

	fdb_transaction_destroy(tr);
	// SQLite needs the short-read code so it can zero-fill and grow the file.
	return short_read ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}

static int fdb_write(sqlite3_file *file, const void *buf, int amt, sqlite3_int64 off) {
	FdbFile *f = (FdbFile *)file;
	const uint8_t *in = buf;

	FDBTransaction *tr;
	if (fdb_database_create_transaction(g_db, &tr)) return SQLITE_IOERR_WRITE;

	// The fence is read in the same transaction as the write, so a writer that lost
	// ownership cannot commit. FoundationDB rejects the transaction if the fence moved
	// between the read and the commit.
	uint8_t fkey[MAX_NAME];
	int fklen = fence_key(fkey, f->name);
	if (read_u64(tr, fkey, fklen) != f->fence) {
		fdb_transaction_destroy(tr);
		return SQLITE_READONLY;
	}

	for (int done = 0; done < amt;) {
		sqlite3_int64 at = off + done;
		uint32_t index = (uint32_t)(at / BLOCK);
		int within = (int)(at % BLOCK);
		int want = amt - done;
		if (want > BLOCK - within) want = BLOCK - within;

		uint8_t key[MAX_NAME];
		int klen = block_key(key, f->name, index);
		uint8_t block[BLOCK];
		memset(block, 0, BLOCK);

		// A partial block keeps the bytes already stored beside the new ones.
		if (within != 0 || want != BLOCK) {
			FDBFuture *fu = fdb_transaction_get(tr, key, klen, 0);
			if (!await(fu)) {
				fdb_bool_t present;
				const uint8_t *val;
				int vlen;
				if (!fdb_future_get_value(fu, &present, &val, &vlen) && present)
					memcpy(block, val, vlen > BLOCK ? BLOCK : (size_t)vlen);
			}
			fdb_future_destroy(fu);
		}

		memcpy(block + within, in + done, (size_t)want);
		fdb_transaction_set(tr, key, klen, block, BLOCK);
		done += want;
	}

	// The size is part of the same transaction, so a reader never sees a block that
	// the size does not cover.
	sqlite3_int64 end = off + amt;
	if (end > read_size(f->name)) {
		uint8_t skey[MAX_NAME], sval[8];
		int sklen = size_key(skey, f->name);
		for (int i = 0; i < 8; i++) sval[i] = (uint8_t)(end >> (56 - 8 * i));
		fdb_transaction_set(tr, skey, sklen, sval, 8);
	}

	FDBFuture *commit = fdb_transaction_commit(tr);
	fdb_error_t err = await(commit);
	fdb_future_destroy(commit);
	fdb_transaction_destroy(tr);
	return err ? SQLITE_IOERR_WRITE : SQLITE_OK;
}

static int fdb_truncate(sqlite3_file *file, sqlite3_int64 size) {
	FdbFile *f = (FdbFile *)file;
	FDBTransaction *tr;
	if (fdb_database_create_transaction(g_db, &tr)) return SQLITE_IOERR_TRUNCATE;

	uint8_t from[MAX_NAME], to[MAX_NAME];
	int flen = block_key(from, f->name, (uint32_t)((size + BLOCK - 1) / BLOCK));
	int tlen = block_key(to, f->name, 0xFFFFFFFFu);
	fdb_transaction_clear_range(tr, from, flen, to, tlen);

	uint8_t skey[MAX_NAME], sval[8];
	int sklen = size_key(skey, f->name);
	for (int i = 0; i < 8; i++) sval[i] = (uint8_t)(size >> (56 - 8 * i));
	fdb_transaction_set(tr, skey, sklen, sval, 8);

	FDBFuture *commit = fdb_transaction_commit(tr);
	fdb_error_t err = await(commit);
	fdb_future_destroy(commit);
	fdb_transaction_destroy(tr);
	return err ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int fdb_file_size(sqlite3_file *file, sqlite3_int64 *out) {
	*out = read_size(((FdbFile *)file)->name);
	return SQLITE_OK;
}

// A commit is already durable when FoundationDB commits it, so sync does nothing.
static int fdb_sync(sqlite3_file *file, int flags) {
	(void)file;
	(void)flags;
	return SQLITE_OK;
}

static int fdb_close(sqlite3_file *file) {
	(void)file;
	return SQLITE_OK;
}

// The actor owns its store and is the single writer, so a lock is not needed.
static int fdb_lock(sqlite3_file *f, int l) { (void)f; (void)l; return SQLITE_OK; }
static int fdb_unlock(sqlite3_file *f, int l) { (void)f; (void)l; return SQLITE_OK; }
static int fdb_check_lock(sqlite3_file *f, int *out) { (void)f; *out = 0; return SQLITE_OK; }
static int fdb_control(sqlite3_file *f, int op, void *arg) {
	(void)f; (void)op; (void)arg;
	return SQLITE_NOTFOUND;
}
static int fdb_sector(sqlite3_file *f) { (void)f; return BLOCK; }
static int fdb_devchar(sqlite3_file *f) {
	(void)f;
	// One write of one block reaches FoundationDB whole, so SQLite may skip work it
	// would otherwise do to survive a torn page.
	return SQLITE_IOCAP_ATOMIC4K | SQLITE_IOCAP_SAFE_APPEND | SQLITE_IOCAP_SEQUENTIAL;
}

static const sqlite3_io_methods FDB_IO = {
	1, fdb_close, fdb_read, fdb_write, fdb_truncate, fdb_sync, fdb_file_size,
	fdb_lock, fdb_unlock, fdb_check_lock, fdb_control, fdb_sector, fdb_devchar,
};

// ── The VFS ───────────────────────────────────────────────────────────────────

static int vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags,
                    int *out_flags) {
	(void)vfs;
	FdbFile *f = (FdbFile *)file;
	memset(f, 0, sizeof(*f));
	f->base.pMethods = &FDB_IO;
	snprintf(f->name, MAX_NAME, "%s", name ? name : "anonymous");
	// Take ownership. A writer that opened earlier now holds a stale fence.
	f->fence = claim_fence(f->name);
	if (out_flags) *out_flags = flags;
	return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *vfs, const char *name, int sync) {
	(void)vfs;
	(void)sync;
	FDBTransaction *tr;
	if (fdb_database_create_transaction(g_db, &tr)) return SQLITE_IOERR_DELETE;

	uint8_t from[MAX_NAME], to[MAX_NAME], skey[MAX_NAME];
	int flen = block_key(from, name, 0);
	int tlen = block_key(to, name, 0xFFFFFFFFu);
	int sklen = size_key(skey, name);
	fdb_transaction_clear_range(tr, from, flen, to, tlen);
	fdb_transaction_clear(tr, skey, sklen);

	FDBFuture *commit = fdb_transaction_commit(tr);
	fdb_error_t err = await(commit);
	fdb_future_destroy(commit);
	fdb_transaction_destroy(tr);
	return err ? SQLITE_IOERR_DELETE : SQLITE_OK;
}

static int vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
	(void)vfs;
	(void)flags;
	*out = read_size(name) > 0;
	return SQLITE_OK;
}

static int vfs_fullpath(sqlite3_vfs *vfs, const char *in, int n, char *out) {
	(void)vfs;
	snprintf(out, (size_t)n, "%s", in);
	return SQLITE_OK;
}

static int vfs_randomness(sqlite3_vfs *vfs, int n, char *out) {
	return sqlite3_vfs_find("unix")->xRandomness(vfs, n, out);
}
static int vfs_sleep(sqlite3_vfs *vfs, int micros) {
	return sqlite3_vfs_find("unix")->xSleep(vfs, micros);
}
static int vfs_time(sqlite3_vfs *vfs, double *out) {
	return sqlite3_vfs_find("unix")->xCurrentTime(vfs, out);
}

static sqlite3_vfs FDB_VFS = {
	.iVersion = 1,
	.szOsFile = sizeof(FdbFile),
	.mxPathname = MAX_NAME,
	.zName = "weft_fdb",
	.xOpen = vfs_open,
	.xDelete = vfs_delete,
	.xAccess = vfs_access,
	.xFullPathname = vfs_fullpath,
	.xRandomness = vfs_randomness,
	.xSleep = vfs_sleep,
	.xCurrentTime = vfs_time,
};

int weft_vfs_register(int make_default) { return sqlite3_vfs_register(&FDB_VFS, make_default); }
