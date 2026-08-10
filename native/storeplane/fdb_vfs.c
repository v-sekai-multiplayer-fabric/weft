// A SQLite VFS whose files live in FoundationDB.
//
// This is the store plane described in docs/reference/native_store_plane.md. There is no
// local file, so an actor's database moves between machines with no copy and no restore
// step.
//
// Layout. rivet's Depot layout, modelled in docs/spec/Store.lean:
//
//   weft/db/<name>/HEAD                 the txid of the newest commit
//   weft/db/<name>/SIZE                 the file size, 8 bytes big endian
//   weft/db/<name>/FENCE                the ownership fence
//   weft/db/<name>/PIDX/<pgno>          the txid that owns a page
//   weft/db/<name>/DELTA/<txid>/<pgno>  the pages of one commit
//   weft/db/<name>/SHARD/<as_of>/<pgno> a compacted base, versioned by as_of
//   weft/db/<name>/SHARDN/<as_of>       the page count of a shard version
//   weft/db/<name>/LOGN                 the page count of the log since compaction
//   weft/db/<name>/PIN/<txid>           a read that holds a shard version
//
// A read finds the owner in PIDX and then reads one of DELTA or SHARD. So a read touches
// two rows whatever the log holds, which `Store.lean` proves as `read_touches_two_rows`.
//
// A commit is one FoundationDB transaction. The VFS holds the pages SQLite writes in
// memory, and it sends them when SQLite syncs the file. The pages of a commit go under
// one txid first, where no read can see them. One transaction then points PIDX at that
// txid and advances the head. A process that dies part way through leaves either the
// whole commit or none of it.
//
// Losing the commit that was in flight is correct. `Weft.Actor.Store` accepts that a
// crash loses the last few commits. Leaving pages from two commits is a different
// failure, because a reader cannot see it and it loses the whole actor.
//
// The journal must stay in memory. Set `PRAGMA journal_mode=MEMORY`. The commit above is
// atomic, so a rollback journal on disk adds cost and protects nothing.
//
// Every transaction runs in the retry loop that FoundationDB documents.
// `fdb_transaction_on_error` decides if an error may be retried, and waits before the
// next attempt. A fence mismatch is not retried, because refusing the write is the
// correct answer.
//
// Locking is a no-op. An actor is the single writer of its own store. The fence, not a
// lock, is what stops a second writer. See the Weft moduledoc.
//
// How to read this file. It has four layers, and each one uses only the layer above it:
//
//   1. Keys.        `key_pidx` and the rest build one key each. A number goes into a key
//                   big endian, so the order of the keys is the order of the numbers.
//   2. Transactions. `run_txn` runs a body and retries it the way FoundationDB asks. Each
//                   body is a small function named `*_body`, and it reads and writes
//                   through the helpers above it. A body can run more than once, so it
//                   must build its own state every time.
//   3. Pages.       `page_from_store` reads one page, and the dirty buffer holds the
//                   pages SQLite wrote but has not committed.
//   4. SQLite.      `fdb_read`, `fdb_write`, `fdb_sync` and the rest are what SQLite
//                   calls. `flush` is where a commit happens.

#define _POSIX_C_SOURCE 200809L

#include <fdb_c.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// SQLite's page size. This is not a choice.
#define PAGE 4096
#define MAX_NAME 512
#define KEYMAX 640

// FoundationDB caps a value and a transaction. These are not choices either. Every
// limit below comes from them, so there is no constant to tune. See `Store.lean`.
#define FDB_TXN_LIMIT 10000000

// How much of a transaction one page costs, counting the key and not only the bytes. A
// limit that counts the pages alone overruns the transaction on the keys.
#define DELTA_ROW (PAGE + KEYMAX)
#define PIDX_ROW (KEYMAX + 8)

// The largest commit that goes in one transaction. Such a commit carries the page and
// its index row, so it pays for both. `Store.lean` derives the same bound as
// `maxCommitPages`.
#define ONE_TXN_PAGES (FDB_TXN_LIMIT / (DELTA_ROW + PIDX_ROW))

// The largest number of pages that one staging transaction carries. Staging writes the
// pages alone, so it pays for the page rows only.
#define STAGE_TXN_PAGES (FDB_TXN_LIMIT / DELTA_ROW)

// The largest number of index rows that fit the transaction that moves the head. This is
// what bounds a commit overall, because the index must land at once.
#define MAX_TXN_PIDX (FDB_TXN_LIMIT / PIDX_ROW)

static FDBDatabase *g_db;
static pthread_t g_network_thread;

// ── Fault injection ───────────────────────────────────────────────────────────
//
// `WEFT_CRASH_AT_COMMIT=N` stops the process before its Nth write transaction commits.
// A crash point must be repeatable, because `prove_crash` searches over crash points and
// a search cannot repeat a failure it cannot address. The hook is inert when the
// variable is not set.

static long g_crash_at;
static long g_commits;

static void crash_point(void) {
	if (g_crash_at && ++g_commits == g_crash_at) {
		// _exit, so nothing is flushed and no handler runs. This is the crash a machine
		// failure gives.
		_exit(9);
	}
}

// ── FoundationDB helpers ──────────────────────────────────────────────────────

static void *run_network(void *unused) {
	(void)unused;
	fdb_run_network();
	return NULL;
}

// Start the client once for the process. `cluster_file` may be NULL for the default.
int weft_fdb_start(const char *cluster_file) {
	const char *at = getenv("WEFT_CRASH_AT_COMMIT");
	g_crash_at = at ? atol(at) : 0;

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

// ── Keys ──────────────────────────────────────────────────────────────────────
//
// A number goes into a key big endian, so the order of the keys is the order of the
// numbers. A range read then gives pages and commits in order.

static void put_be32(uint8_t *out, uint32_t v) {
	for (int i = 0; i < 4; i++) out[i] = (uint8_t)(v >> (24 - 8 * i));
}

static void put_be64(uint8_t *out, uint64_t v) {
	for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint64_t get_be64(const uint8_t *in) {
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v = (v << 8) | in[i];
	return v;
}

static int key_meta(uint8_t *out, const char *name, const char *what) {
	return snprintf((char *)out, KEYMAX, "weft/db/%s/%s", name, what);
}

static int key_pidx(uint8_t *out, const char *name, uint32_t pgno) {
	int n = snprintf((char *)out, KEYMAX, "weft/db/%s/PIDX/", name);
	put_be32(out + n, pgno);
	return n + 4;
}

static int key_delta(uint8_t *out, const char *name, uint64_t txid, uint32_t pgno) {
	int n = snprintf((char *)out, KEYMAX, "weft/db/%s/DELTA/", name);
	put_be64(out + n, txid);
	put_be32(out + n + 8, pgno);
	return n + 12;
}

static int key_delta_txid(uint8_t *out, const char *name, uint64_t txid) {
	int n = snprintf((char *)out, KEYMAX, "weft/db/%s/DELTA/", name);
	put_be64(out + n, txid);
	return n + 8;
}

static int key_shard(uint8_t *out, const char *name, uint64_t as_of, uint32_t pgno) {
	int n = snprintf((char *)out, KEYMAX, "weft/db/%s/SHARD/", name);
	put_be64(out + n, as_of);
	put_be32(out + n + 8, pgno);
	return n + 12;
}

static int key_shard_version(uint8_t *out, const char *name, uint64_t as_of) {
	int n = snprintf((char *)out, KEYMAX, "weft/db/%s/SHARD/", name);
	put_be64(out + n, as_of);
	return n + 8;
}

static int key_shardn(uint8_t *out, const char *name, uint64_t as_of) {
	int n = snprintf((char *)out, KEYMAX, "weft/db/%s/SHARDN/", name);
	put_be64(out + n, as_of);
	return n + 8;
}

static int key_prefix(uint8_t *out, const char *name, const char *what) {
	return snprintf((char *)out, KEYMAX, "weft/db/%s/%s/", name, what);
}

// The first key after every key with this prefix. FoundationDB calls it strinc. A range
// from a prefix to this covers exactly the keys under that prefix.
static int key_after(uint8_t *out, const uint8_t *prefix, int plen) {
	if (out != prefix) memcpy(out, prefix, (size_t)plen);
	int n = plen;
	while (n > 0 && out[n - 1] == 0xFF) n--;
	if (n > 0) out[n - 1]++;
	return n;
}

// ── The transaction runner ────────────────────────────────────────────────────

// A transaction body. It returns a FoundationDB error, or zero. It sets `final` to a
// SQLite code when the failure must reach the caller instead of being retried.
//
// A body runs again after a retryable error, so a body must build its own state and must
// not depend on what an earlier attempt did.
typedef fdb_error_t (*txn_body)(FDBTransaction *tr, void *ctx, int *final);

static int run_txn(txn_body body, void *ctx, int is_write, int ioerr) {
	FDBTransaction *tr = NULL;
	if (fdb_database_create_transaction(g_db, &tr)) return ioerr;

	for (;;) {
		int final = 0;
		fdb_error_t err = body(tr, ctx, &final);
		if (final) {
			fdb_transaction_destroy(tr);
			return final;
		}
		if (!err) {
			if (!is_write) {
				// A read needs no commit. Ending it here releases the read version.
				fdb_transaction_destroy(tr);
				return SQLITE_OK;
			}
			crash_point();
			FDBFuture *c = fdb_transaction_commit(tr);
			err = await(c);
			fdb_future_destroy(c);
			if (!err) {
				fdb_transaction_destroy(tr);
				return SQLITE_OK;
			}
		}

		// FoundationDB decides whether the error may be retried, and waits the right
		// amount before the next attempt. It resets the transaction when it may be
		// retried, and it returns the error when it may not.
		FDBFuture *r = fdb_transaction_on_error(tr, err);
		fdb_error_t fatal = await(r);
		fdb_future_destroy(r);
		if (fatal) {
			fdb_transaction_destroy(tr);
			return ioerr;
		}
	}
}

// ── Reads inside a transaction ────────────────────────────────────────────────

static fdb_error_t get_bytes(FDBTransaction *tr, const uint8_t *key, int klen, uint8_t *out,
                             int max, int *len, int *present) {
	FDBFuture *f = fdb_transaction_get(tr, key, klen, 0);
	fdb_error_t err = await(f);
	if (!err) {
		fdb_bool_t got;
		const uint8_t *val;
		int vlen;
		err = fdb_future_get_value(f, &got, &val, &vlen);
		if (!err) {
			*present = got;
			if (got) {
				int n = vlen > max ? max : vlen;
				memcpy(out, val, (size_t)n);
				if (len) *len = n;
			}
		}
	}
	fdb_future_destroy(f);
	return err;
}

static fdb_error_t get_u64(FDBTransaction *tr, const uint8_t *key, int klen, uint64_t *out,
                           int *present) {
	uint8_t val[8];
	int len = 0, got = 0;
	fdb_error_t err = get_bytes(tr, key, klen, val, 8, &len, &got);
	if (err) return err;
	*present = got;
	if (got && len == 8) *out = get_be64(val);
	else *present = 0;
	return 0;
}

static void set_u64(FDBTransaction *tr, const uint8_t *key, int klen, uint64_t v) {
	uint8_t val[8];
	put_be64(val, v);
	fdb_transaction_set(tr, key, klen, val, 8);
}

// Read the number at the end of the first or the last key of a range. Every versioned
// key in this layout ends with its number, so this is how the VFS asks for the newest
// shard version or the oldest pin. `found` stays 0 when the range is empty.
static fdb_error_t edge_number(FDBTransaction *tr, const uint8_t *begin, int blen,
                               const uint8_t *end, int elen, int want_last, uint64_t *out,
                               int *found) {
	*found = 0;
	FDBFuture *f = fdb_transaction_get_range(tr, FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(begin, blen),
	                                         FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(end, elen), 1, 0,
	                                         FDB_STREAMING_MODE_WANT_ALL, 1, 0, want_last);
	fdb_error_t err = await(f);
	if (!err) {
		const FDBKeyValue *kv;
		int count;
		fdb_bool_t more;
		err = fdb_future_get_keyvalue_array(f, &kv, &count, &more);
		if (!err && count > 0 && kv[0].key_length >= 8) {
			*out = get_be64(kv[0].key + kv[0].key_length - 8);
			*found = 1;
		}
	}
	fdb_future_destroy(f);
	return err;
}

// Clear every key under `weft/db/<name>/<what>/`. FoundationDB stores a range clear as
// one small record, so the cost does not grow with the number of keys it removes.
static void clear_prefix(FDBTransaction *tr, const char *name, const char *what) {
	uint8_t from[KEYMAX], to[KEYMAX];
	int flen = key_prefix(from, name, what);
	int tlen = key_after(to, from, flen);
	fdb_transaction_clear_range(tr, from, flen, to, tlen);
}

// ── The file ──────────────────────────────────────────────────────────────────

// One page that SQLite wrote and the store has not taken yet.
typedef struct {
	uint32_t pgno;
	uint8_t bytes[PAGE];
} DirtyPage;

typedef struct {
	sqlite3_file base;
	char name[MAX_NAME];
	int64_t fence;

	uint64_t head;        // the newest commit
	uint64_t shard_as_of; // the newest shard version
	int has_shard;

	// The page counts that decide when to compact. The single writer owns both, so it
	// keeps them here and does not read them back. A round trip for a number this
	// process already knows is the most expensive way to learn nothing.
	uint64_t base_pages; // the pages of the newest shard version
	uint64_t log_pages;  // the pages committed since that version

	int64_t size;      // the file size, counting what is buffered
	int64_t sent_size; // the file size the store holds

	// The pages SQLite wrote, sorted by page number. They are the truth for a read
	// until a sync sends them.
	DirtyPage **dirty;
	int ndirty, capdirty;

	// The page count at or above which every page is dropped by the pending truncate.
	// -1 means no truncate is pending.
	int64_t trunc_pages;
} FdbFile;

// Find the slot of `pgno`, or where it would go. Returns 1 when it is there.
static int find_dirty(FdbFile *f, uint32_t pgno, int *slot) {
	int lo = 0, hi = f->ndirty;
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;
		if (f->dirty[mid]->pgno < pgno) lo = mid + 1;
		else hi = mid;
	}
	*slot = lo;
	return lo < f->ndirty && f->dirty[lo]->pgno == pgno;
}

static DirtyPage *insert_dirty(FdbFile *f, uint32_t pgno, int slot) {
	if (f->ndirty == f->capdirty) {
		int cap = f->capdirty ? f->capdirty * 2 : 32;
		DirtyPage **grown = realloc(f->dirty, (size_t)cap * sizeof(*grown));
		if (!grown) return NULL;
		f->dirty = grown;
		f->capdirty = cap;
	}
	DirtyPage *p = calloc(1, sizeof(*p));
	if (!p) return NULL;
	p->pgno = pgno;
	memmove(&f->dirty[slot + 1], &f->dirty[slot],
	        (size_t)(f->ndirty - slot) * sizeof(*f->dirty));
	f->dirty[slot] = p;
	f->ndirty++;
	return p;
}

static void clear_dirty(FdbFile *f) {
	for (int i = 0; i < f->ndirty; i++) free(f->dirty[i]);
	f->ndirty = 0;
	f->trunc_pages = -1;
}

// Read one page from the store. `present` stays 0 when no commit and no shard holds it,
// and the caller then zero-fills.
static fdb_error_t page_from_store(FDBTransaction *tr, FdbFile *f, uint32_t pgno,
                                   uint8_t *out, int *len, int *present) {
	uint8_t key[KEYMAX];
	uint64_t owner = 0;
	int got = 0;

	*present = 0;
	*len = 0;

	int klen = key_pidx(key, f->name, pgno);
	fdb_error_t err = get_u64(tr, key, klen, &owner, &got);
	if (err) return err;

	if (got) {
		klen = key_delta(key, f->name, owner, pgno);
		return get_bytes(tr, key, klen, out, PAGE, len, present);
	}
	if (f->has_shard) {
		klen = key_shard(key, f->name, f->shard_as_of, pgno);
		return get_bytes(tr, key, klen, out, PAGE, len, present);
	}
	return 0;
}

// ── Open ──────────────────────────────────────────────────────────────────────

struct open_ctx {
	FdbFile *f;
};

// Where the database stands: the newest commit and the file size.
static fdb_error_t load_head(FDBTransaction *tr, FdbFile *file) {
	uint8_t key[KEYMAX];
	uint64_t size = 0;
	int got = 0;
	fdb_error_t err;

	int klen = key_meta(key, file->name, "HEAD");
	if ((err = get_u64(tr, key, klen, &file->head, &got))) return err;

	klen = key_meta(key, file->name, "SIZE");
	if ((err = get_u64(tr, key, klen, &size, &got))) return err;
	file->size = (int64_t)size;
	return 0;
}

// The newest shard version at or below the head. `Store.lean` calls this shardAt, and it
// is the base a read falls through to when no commit owns the page.
static fdb_error_t load_newest_shard(FDBTransaction *tr, FdbFile *file) {
	uint8_t from[KEYMAX], to[KEYMAX], key[KEYMAX];
	uint64_t as_of = 0;
	int found = 0;

	int flen = key_prefix(from, file->name, "SHARDN");
	int tlen = key_shardn(to, file->name, file->head + 1);
	fdb_error_t err = edge_number(tr, from, flen, to, tlen, 1, &as_of, &found);
	if (err) return err;

	file->has_shard = found;
	file->shard_as_of = found ? as_of : 0;

	// The sizes that decide when to compact are read once, here, and then kept in step
	// by the commit and by compaction.
	file->base_pages = 0;
	if (found) {
		int got = 0;
		int klen = key_shardn(key, file->name, as_of);
		if ((err = get_u64(tr, key, klen, &file->base_pages, &got))) return err;
		if (!got) file->base_pages = 0;
	}

	int got = 0;
	int klen = key_meta(key, file->name, "LOGN");
	if ((err = get_u64(tr, key, klen, &file->log_pages, &got))) return err;
	if (!got) file->log_pages = 0;
	return 0;
}

// Drop the pages of a commit that never finished.
//
// A commit writes its pages under a txid above the head before it advances the head. A
// process that died between the two leaves those pages behind. They are unreachable,
// because no PIDX row points at them, so clearing them frees space and changes no read.
static void drop_unfinished_commit(FDBTransaction *tr, FdbFile *file) {
	uint8_t from[KEYMAX], to[KEYMAX];
	int flen = key_delta_txid(from, file->name, file->head + 1);
	int plen = key_prefix(to, file->name, "DELTA");
	int tlen = key_after(to, to, plen);
	fdb_transaction_clear_range(tr, from, flen, to, tlen);
}

// Take ownership. A writer that opened earlier now holds a stale number, and its next
// write is refused.
static fdb_error_t raise_fence(FDBTransaction *tr, FdbFile *file) {
	uint8_t key[KEYMAX];
	uint64_t fence = 0;
	int got = 0;

	int klen = key_meta(key, file->name, "FENCE");
	fdb_error_t err = get_u64(tr, key, klen, &fence, &got);
	if (err) return err;

	fence = got ? fence + 1 : 1;
	set_u64(tr, key, klen, fence);
	file->fence = (int64_t)fence;
	return 0;
}

static fdb_error_t open_body(FDBTransaction *tr, void *ctx, int *final) {
	(void)final;
	FdbFile *file = ((struct open_ctx *)ctx)->f;
	fdb_error_t err;

	// An attempt may run again, so it starts from a known state.
	file->head = 0;
	file->size = 0;
	file->has_shard = 0;
	file->shard_as_of = 0;

	if ((err = load_head(tr, file))) return err;
	if ((err = load_newest_shard(tr, file))) return err;
	drop_unfinished_commit(tr, file);
	if ((err = raise_fence(tr, file))) return err;

	file->sent_size = file->size;
	return 0;
}

// ── Read ──────────────────────────────────────────────────────────────────────

struct read_ctx {
	FdbFile *f;
	uint8_t *out;
	int amt;
	sqlite3_int64 off;
	int short_read;
};

static fdb_error_t read_body(FDBTransaction *tr, void *ctx, int *final) {
	(void)final;
	struct read_ctx *r = ctx;
	FdbFile *f = r->f;

	// An attempt starts over, so clear what an earlier one wrote.
	memset(r->out, 0, (size_t)r->amt);
	r->short_read = 0;

	for (int done = 0; done < r->amt;) {
		sqlite3_int64 at = r->off + done;
		uint32_t pgno = (uint32_t)(at / PAGE);
		int within = (int)(at % PAGE);
		int want = r->amt - done;
		if (want > PAGE - within) want = PAGE - within;

		// A page SQLite wrote and the store has not taken yet is the truth.
		int slot;
		if (find_dirty(f, pgno, &slot)) {
			memcpy(r->out + done, f->dirty[slot]->bytes + within, (size_t)want);
			done += want;
			continue;
		}

		uint8_t page[PAGE];
		int len = 0, present = 0;
		fdb_error_t err = page_from_store(tr, f, pgno, page, &len, &present);
		if (err) return err;

		if (present) {
			int have = len - within;
			if (have > 0) memcpy(r->out + done, page + within, have < want ? (size_t)have : (size_t)want);
			if (have < want) r->short_read = 1;
		} else {
			r->short_read = 1;
		}
		done += want;
	}
	return 0;
}

static int fdb_read(sqlite3_file *file, void *buf, int amt, sqlite3_int64 off) {
	struct read_ctx r = {(FdbFile *)file, buf, amt, off, 0};
	int rc = run_txn(read_body, &r, 0, SQLITE_IOERR_READ);
	if (rc != SQLITE_OK) return rc;
	// SQLite needs the short-read code so it can zero-fill and grow the file.
	return r.short_read ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}

// ── Write ─────────────────────────────────────────────────────────────────────

// A write goes into memory. The store takes it when SQLite syncs, so the pages of one
// SQLite commit reach FoundationDB as one transaction.
//
// A write that covers part of a page has to keep the bytes already stored beside the new
// ones, so it reads that page first.
// Give back the buffered copy of a page, adding it when it is not there yet.
//
// `whole` says the caller is about to overwrite every byte of the page, so the page in
// the store does not have to be read first. A write that covers only part of a page has
// to keep the bytes already stored beside the new ones.
static DirtyPage *buffer_page(FdbFile *file, uint32_t pgno, int whole, int *rc) {
	int slot;
	*rc = SQLITE_OK;

	if (find_dirty(file, pgno, &slot)) return file->dirty[slot];

	if (file->ndirty >= MAX_TXN_PIDX) {
		// The index rows of this commit no longer fit one transaction, so the commit
		// could not be made atomic.
		*rc = SQLITE_IOERR_WRITE;
		return NULL;
	}

	// Read the stored page before the buffer holds it, so the read goes to the store.
	uint8_t stored[PAGE];
	memset(stored, 0, PAGE);
	if (!whole) {
		struct read_ctx r = {file, stored, PAGE, (sqlite3_int64)pgno * PAGE, 0};
		int err = run_txn(read_body, &r, 0, SQLITE_IOERR_READ);
		if (err != SQLITE_OK && err != SQLITE_IOERR_SHORT_READ) {
			*rc = err;
			return NULL;
		}
	}

	DirtyPage *page = insert_dirty(file, pgno, slot);
	if (!page) {
		*rc = SQLITE_IOERR_NOMEM;
		return NULL;
	}
	if (!whole) memcpy(page->bytes, stored, PAGE);
	return page;
}

static int fdb_write(sqlite3_file *file, const void *buf, int amt, sqlite3_int64 off) {
	FdbFile *f = (FdbFile *)file;
	const uint8_t *in = buf;

	for (int done = 0; done < amt;) {
		sqlite3_int64 at = off + done;
		uint32_t pgno = (uint32_t)(at / PAGE);
		int within = (int)(at % PAGE);
		int want = amt - done;
		if (want > PAGE - within) want = PAGE - within;

		int rc = SQLITE_OK;
		DirtyPage *page = buffer_page(f, pgno, within == 0 && want == PAGE, &rc);
		if (!page) return rc;

		memcpy(page->bytes + within, in + done, (size_t)want);
		done += want;
	}

	if (off + amt > f->size) f->size = off + amt;
	return SQLITE_OK;
}

// ── Commit ────────────────────────────────────────────────────────────────────

struct flush_ctx {
	FdbFile *f;
	uint64_t txid;
	int lo, hi; // the pages this transaction carries
};

// Refuse the transaction unless this handle still owns the database.
//
// Every write transaction reads the fence, not only the commit. A writer that lost
// ownership must not compact either, because compaction drops the shard version that the
// owner is reading. Reading the fence here also makes FoundationDB reject the
// transaction if the fence moves before it commits.
static fdb_error_t check_fence(FDBTransaction *tr, FdbFile *file, int *final) {
	uint8_t key[KEYMAX];
	uint64_t fence = 0;
	int got = 0;

	int klen = key_meta(key, file->name, "FENCE");
	fdb_error_t err = get_u64(tr, key, klen, &fence, &got);
	if (err) return err;

	// Refusing the write is the correct answer, so it must reach the caller instead of
	// being retried.
	if (!got || (int64_t)fence != file->fence) *final = SQLITE_READONLY;
	return 0;
}

// The pages of the commit, under a txid no read can reach yet.
static void put_delta_pages(FDBTransaction *tr, struct flush_ctx *c) {
	uint8_t key[KEYMAX];
	for (int i = c->lo; i < c->hi; i++) {
		int klen = key_delta(key, c->f->name, c->txid, c->f->dirty[i]->pgno);
		fdb_transaction_set(tr, key, klen, c->f->dirty[i]->bytes, PAGE);
	}
}

static fdb_error_t delta_body(FDBTransaction *tr, void *ctx, int *final) {
	struct flush_ctx *c = ctx;
	fdb_error_t err = check_fence(tr, c->f, final);
	if (err || *final) return err;
	put_delta_pages(tr, c);
	return 0;
}

// What makes a commit visible: PIDX points at the new txid, the size moves, and the head
// advances. All of it lands together or none of it does.
static fdb_error_t put_head(FDBTransaction *tr, struct flush_ctx *c) {
	FdbFile *f = c->f;
	uint8_t key[KEYMAX], to[KEYMAX];
	fdb_error_t err;
	int got = 0;
	int klen;

	for (int i = 0; i < f->ndirty; i++) {
		klen = key_pidx(key, f->name, f->dirty[i]->pgno);
		set_u64(tr, key, klen, c->txid);
	}

	// A truncate drops the index rows of the pages it removed. The pages themselves
	// stay where they are, unreachable, and the next compaction does not fold them.
	if (f->trunc_pages >= 0) {
		klen = key_pidx(key, f->name, (uint32_t)f->trunc_pages);
		int plen = key_prefix(to, f->name, "PIDX");
		int tlen = key_after(to, to, plen);
		fdb_transaction_clear_range(tr, key, klen, to, tlen);
	}

	klen = key_meta(key, f->name, "SIZE");
	set_u64(tr, key, klen, (uint64_t)f->size);

	// The absolute value, not a read and a sum. A single writer knows what the log holds,
	// so reading it back would cost a round trip to learn a number it already has.
	klen = key_meta(key, f->name, "LOGN");
	set_u64(tr, key, klen, f->log_pages + (uint64_t)f->ndirty);

	klen = key_meta(key, f->name, "HEAD");
	set_u64(tr, key, klen, c->txid);
	return 0;
}

static fdb_error_t head_body(FDBTransaction *tr, void *ctx, int *final) {
	struct flush_ctx *c = ctx;
	fdb_error_t err = check_fence(tr, c->f, final);
	if (err || *final) return err;
	return put_head(tr, c);
}

// The whole commit in one transaction: the pages and the head together.
//
// A commit that fits one transaction needs no staging. The pages are never visible
// before the head moves, because they arrive with it.
static fdb_error_t commit_body(FDBTransaction *tr, void *ctx, int *final) {
	struct flush_ctx *c = ctx;
	fdb_error_t err = check_fence(tr, c->f, final);
	if (err || *final) return err;
	put_delta_pages(tr, c);
	return put_head(tr, c);
}

static int compact(FdbFile *f);
static int should_compact(FdbFile *f);

// Send everything SQLite wrote as one commit.
//
// A commit is one transaction whenever it fits one, which is the common case and the
// cheap one. It costs a single round trip, and there is no window where the pages exist
// and the head does not.
//
// A commit too large for one transaction stages instead. The pages go first, under a
// txid no read can reach, and one more transaction then moves the head. This is the
// shape CockroachDB calls a parallel commit: the writes are staged, and the commit is
// the single record that makes them real. The staged pages are safe to leave behind,
// because `drop_unfinished_commit` clears any txid above the head at the next open.
static int flush(FdbFile *f) {
	if (f->ndirty == 0 && f->trunc_pages < 0 && f->size == f->sent_size) return SQLITE_OK;

	struct flush_ctx c = {f, f->head + 1, 0, f->ndirty};
	int rc;

	if (f->ndirty <= ONE_TXN_PAGES) {
		rc = run_txn(commit_body, &c, 1, SQLITE_IOERR_WRITE);
		if (rc != SQLITE_OK) return rc;
	} else {
		for (int lo = 0; lo < f->ndirty; lo += STAGE_TXN_PAGES) {
			c.lo = lo;
			c.hi = lo + STAGE_TXN_PAGES;
			if (c.hi > f->ndirty) c.hi = f->ndirty;
			rc = run_txn(delta_body, &c, 1, SQLITE_IOERR_WRITE);
			if (rc != SQLITE_OK) return rc;
		}
		rc = run_txn(head_body, &c, 1, SQLITE_IOERR_WRITE);
		if (rc != SQLITE_OK) return rc;
	}

	f->head = c.txid;
	f->sent_size = f->size;
	f->log_pages += (uint64_t)f->ndirty;
	clear_dirty(f);

	return should_compact(f) ? compact(f) : SQLITE_OK;
}

// ── Compaction ────────────────────────────────────────────────────────────────
//
// Compaction folds the log into a new shard version. It adds a version and never
// overwrites one, and it clears a PIDX row only when that row points at a folded txid.
// `Store.lean` proves that these two rules preserve every read.
//
// The trigger is a ratio, not a number: fold when the log is as large as the base. A
// ratio has no units to tune, and it moves with the load. A quiet actor never compacts.

// Compact when the log is as large as the base. `Store.lean` calls this shouldCompact.
//
// Both sizes live in the handle, so the decision costs nothing. It used to cost two round
// trips after every commit, which was more than the commit itself.
static int should_compact(FdbFile *f) {
	return f->log_pages > 0 && f->base_pages <= f->log_pages;
}

struct fold_ctx {
	FdbFile *f;
	uint64_t as_of;
	uint32_t lo, hi;  // the window of pages this pass folds
	uint8_t *pages;   // hi - lo pages
	uint8_t *present; // one flag for each page in the window
	uint32_t kept;
};

// Read a window of pages through the read path, so the fold sees exactly what a reader
// sees. Memory stays bounded by one window, so compaction does not load the database.
static fdb_error_t fold_read_body(FDBTransaction *tr, void *ctx, int *final) {
	(void)final;
	struct fold_ctx *c = ctx;
	memset(c->present, 0, c->hi - c->lo);

	for (uint32_t pgno = c->lo; pgno < c->hi; pgno++) {
		int len = 0, present = 0;
		uint8_t *slot = c->pages + (size_t)(pgno - c->lo) * PAGE;
		memset(slot, 0, PAGE);
		fdb_error_t err = page_from_store(tr, c->f, pgno, slot, &len, &present);
		if (err) return err;
		c->present[pgno - c->lo] = (uint8_t)present;
	}
	return 0;
}

static fdb_error_t fold_write_body(FDBTransaction *tr, void *ctx, int *final) {
	struct fold_ctx *c = ctx;
	uint8_t key[KEYMAX];

	fdb_error_t err = check_fence(tr, c->f, final);
	if (err || *final) return err;

	for (uint32_t pgno = c->lo; pgno < c->hi; pgno++) {
		if (!c->present[pgno - c->lo]) continue;
		int klen = key_shard(key, c->f->name, c->as_of, pgno);
		fdb_transaction_set(tr, key, klen, c->pages + (size_t)(pgno - c->lo) * PAGE, PAGE);
	}
	return 0;
}

struct finish_ctx {
	FdbFile *f;
	uint64_t as_of;
	uint32_t kept;
	uint64_t oldest_pin;
};

// Make the new shard version the one a read uses, and drop what it replaced. One
// transaction, so a reader sees the old version or the new one.
static fdb_error_t finish_body(FDBTransaction *tr, void *ctx, int *final) {
	struct finish_ctx *c = ctx;
	FdbFile *f = c->f;
	uint8_t key[KEYMAX], from[KEYMAX], to[KEYMAX];

	fdb_error_t err = check_fence(tr, f, final);
	if (err || *final) return err;

	// The marker that makes the version usable. Until this lands, a read keeps the old
	// base and the log.
	int klen = key_shardn(key, f->name, c->as_of);
	set_u64(tr, key, klen, c->kept);

	// Every PIDX row points at a folded txid, because the fold covered the head. So the
	// whole index goes, and every page now comes from the new shard.
	clear_prefix(tr, f->name, "PIDX");
	clear_prefix(tr, f->name, "DELTA");

	// Retention follows demand. A shard version below the oldest pin is one that nobody
	// can still read, so it goes. With no pin, only the new version is kept.
	uint64_t keep_from = c->oldest_pin < c->as_of ? c->oldest_pin : c->as_of;
	int flen = key_shard_version(from, f->name, 0);
	int tlen = key_shard_version(to, f->name, keep_from);
	fdb_transaction_clear_range(tr, from, flen, to, tlen);
	flen = key_shardn(from, f->name, 0);
	tlen = key_shardn(to, f->name, keep_from);
	fdb_transaction_clear_range(tr, from, flen, to, tlen);

	klen = key_meta(key, f->name, "LOGN");
	set_u64(tr, key, klen, 0);
	return 0;
}

struct pin_ctx {
	FdbFile *f;
	uint64_t oldest;
};

// The oldest read that still holds a shard version. Nothing writes a pin yet, so this
// finds none and compaction keeps only the version it just made.
static fdb_error_t pin_body(FDBTransaction *tr, void *ctx, int *final) {
	(void)final;
	struct pin_ctx *p = ctx;
	uint8_t from[KEYMAX], to[KEYMAX];
	uint64_t oldest = 0;
	int found = 0;

	int flen = key_prefix(from, p->f->name, "PIN");
	int tlen = key_after(to, from, flen);
	fdb_error_t err = edge_number(tr, from, flen, to, tlen, 0, &oldest, &found);
	if (err) return err;

	p->oldest = found ? oldest : UINT64_MAX;
	return 0;
}

static int compact(FdbFile *f) {
	uint64_t as_of = f->head;
	uint32_t npages = (uint32_t)((f->size + PAGE - 1) / PAGE);
	if (npages == 0) return SQLITE_OK;

	// A window is as many pages as one transaction carries, so memory and transaction
	// size come from the same derived limit.
	uint32_t window = STAGE_TXN_PAGES;
	if (window > npages) window = npages;

	struct fold_ctx c = {f, as_of, 0, 0, NULL, NULL, 0};
	c.pages = malloc((size_t)window * PAGE);
	c.present = malloc(window);
	if (!c.pages || !c.present) {
		free(c.pages);
		free(c.present);
		return SQLITE_IOERR_NOMEM;
	}

	uint32_t kept = 0;
	int rc = SQLITE_OK;
	for (uint32_t lo = 0; lo < npages; lo += window) {
		c.lo = lo;
		c.hi = lo + window;
		if (c.hi > npages) c.hi = npages;

		if ((rc = run_txn(fold_read_body, &c, 0, SQLITE_IOERR_READ)) != SQLITE_OK) break;
		if ((rc = run_txn(fold_write_body, &c, 1, SQLITE_IOERR_WRITE)) != SQLITE_OK) break;
		for (uint32_t i = 0; i < c.hi - c.lo; i++)
			if (c.present[i]) kept++;
	}
	free(c.pages);
	free(c.present);
	if (rc != SQLITE_OK) return rc;

	struct pin_ctx p = {f, UINT64_MAX};
	if ((rc = run_txn(pin_body, &p, 0, SQLITE_IOERR_READ)) != SQLITE_OK) return rc;

	struct finish_ctx fin = {f, as_of, kept, p.oldest};
	if ((rc = run_txn(finish_body, &fin, 1, SQLITE_IOERR_WRITE)) != SQLITE_OK) return rc;

	f->has_shard = 1;
	f->shard_as_of = as_of;
	f->base_pages = kept;
	f->log_pages = 0;
	return SQLITE_OK;
}

// ── The rest of the file methods ──────────────────────────────────────────────

static int fdb_truncate(sqlite3_file *file, sqlite3_int64 size) {
	FdbFile *f = (FdbFile *)file;
	int64_t npages = (size + PAGE - 1) / PAGE;

	// Drop the buffered pages the truncate removes.
	int slot;
	find_dirty(f, (uint32_t)npages, &slot);
	for (int i = slot; i < f->ndirty; i++) free(f->dirty[i]);
	f->ndirty = slot;

	if (f->trunc_pages < 0 || npages < f->trunc_pages) f->trunc_pages = npages;
	f->size = size;
	return SQLITE_OK;
}

// SQLite syncs the database when a commit is complete, so this is where the commit goes
// to FoundationDB. A commit is durable when FoundationDB commits it, so there is nothing
// else for a sync to do.
static int fdb_sync(sqlite3_file *file, int flags) {
	(void)flags;
	return flush((FdbFile *)file);
}

static int fdb_file_size(sqlite3_file *file, sqlite3_int64 *out) {
	*out = ((FdbFile *)file)->size;
	return SQLITE_OK;
}

static int fdb_close(sqlite3_file *file) {
	FdbFile *f = (FdbFile *)file;
	int rc = flush(f);
	clear_dirty(f);
	free(f->dirty);
	f->dirty = NULL;
	f->capdirty = 0;
	return rc;
}

// The actor owns its store and is the single writer, so a lock is not needed.
static int fdb_lock(sqlite3_file *f, int l) { (void)f; (void)l; return SQLITE_OK; }
static int fdb_unlock(sqlite3_file *f, int l) { (void)f; (void)l; return SQLITE_OK; }
static int fdb_check_lock(sqlite3_file *f, int *out) { (void)f; *out = 0; return SQLITE_OK; }

static int fdb_control(sqlite3_file *file, int op, void *arg) {
	(void)arg;
	// SQLite reports the end of a commit here. A connection with synchronous off never
	// syncs, so without this the commit would sit in memory.
	if (op == SQLITE_FCNTL_COMMIT_PHASETWO) return flush((FdbFile *)file);
	return SQLITE_NOTFOUND;
}

static int fdb_sector(sqlite3_file *f) { (void)f; return PAGE; }

static int fdb_devchar(sqlite3_file *f) {
	(void)f;
	// One page reaches FoundationDB whole, and a commit reaches it as one transaction.
	// So SQLite may skip the work it would otherwise do to survive a torn page.
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
	f->trunc_pages = -1;
	snprintf(f->name, MAX_NAME, "%s", name ? name : "anonymous");

	struct open_ctx c = {f};
	int rc = run_txn(open_body, &c, 1, SQLITE_IOERR);
	if (rc != SQLITE_OK) return rc;

	if (out_flags) *out_flags = flags;
	return SQLITE_OK;
}

struct delete_ctx {
	const char *name;
};

static fdb_error_t delete_body(FDBTransaction *tr, void *ctx, int *final) {
	(void)final;
	const char *name = ((struct delete_ctx *)ctx)->name;
	uint8_t from[KEYMAX], to[KEYMAX];
	int plen = snprintf((char *)from, KEYMAX, "weft/db/%s/", name);
	int tlen = key_after(to, from, plen);
	fdb_transaction_clear_range(tr, from, plen, to, tlen);
	return 0;
}

static int vfs_delete(sqlite3_vfs *vfs, const char *name, int sync) {
	(void)vfs;
	(void)sync;
	struct delete_ctx c = {name};
	return run_txn(delete_body, &c, 1, SQLITE_IOERR_DELETE);
}

struct access_ctx {
	const char *name;
	int exists;
};

static fdb_error_t access_body(FDBTransaction *tr, void *ctx, int *final) {
	(void)final;
	struct access_ctx *a = ctx;
	uint8_t key[KEYMAX];
	uint64_t size = 0;
	int got = 0;
	int klen = key_meta(key, a->name, "SIZE");
	fdb_error_t err = get_u64(tr, key, klen, &size, &got);
	if (err) return err;
	a->exists = got && size > 0;
	return 0;
}

static int vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
	(void)vfs;
	(void)flags;
	struct access_ctx c = {name, 0};
	int rc = run_txn(access_body, &c, 0, SQLITE_IOERR);
	*out = rc == SQLITE_OK ? c.exists : 0;
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
