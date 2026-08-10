# native store plane

Goal: the store plane is a native process. SQLite runs inside it with a custom VFS, and
that VFS reads and writes pages in FoundationDB. The BEAM reaches the plane over Eclipse
iceoryx v1. The plane reaches FoundationDB with the native client, `libfdb_c`.

```
BEAM control plane
  |  iceoryx v1, zero copy, same machine
store plane (native)
  |  SQLite + custom VFS
  |  page reads and commits
  |  libfdb_c, over the network
FoundationDB
```

State: not started. The Elixir prototype, `Weft.Actor.Store.Replicated` and
`.Replicator`, passes three FoundationDB tests. It is not this design. It uses SQLite as
a key and value table, it replicates logical rows rather than pages, and
`Weft.Actor.load_all` reads the whole actor into memory when the actor starts.

## Why a VFS

A VFS gives SQLite its pages one page at a time. So SQLite reads the pages a query
touches and no others. Three things follow, and none of them is true of the prototype.

- **An actor is not limited by memory.** The working set is in memory, and the rest is in
  FoundationDB. This is what makes the 10 GiB limit in `actor_limits.md` possible.
- **A handoff copies nothing.** A different machine opens the same database and reads
  pages. There is no restore step and no transfer, so a large actor moves as fast as a
  small one.
- **Compaction is not weft's to get wrong.** The Elixir replicator folds a log by hand,
  and it has a race. `../spec/Store.lean` proves the rule the fold must obey. The page
  layout moves that work into one place with one owner.

## The layout

rivet's Depot layout, modelled in `../spec/Store.lean`:

- `PIDX/{pgno}` gives the txid that owns a page, so a read never scans the log.
- `DELTA/{txid}/{chunk}` holds the pages of one commit.
- `SHARD/{shard}/{as_of_txid}` holds a compacted base, versioned. Compaction adds a
  version and never overwrites one.

`../spec/Store.lean` proves that compaction preserves every read, that the in-place fold
loses a page, and that a read touches two rows whatever the log holds.

## Read-ahead is the engineering

A page miss is a network round trip, so a VFS over FoundationDB lives or dies on
read-ahead. rivet's VFS is 3473 lines, and most of it is this.

It does not pick a depth. It scores the access pattern:

```
forward page, gap of 8 or less   score + 2, capped at 12
random page, a scan is running   score - 1
random page, no scan             score - 4
score of 6 or more               escalate read-ahead
score of 10 or more              full depth, 256 pages or 1 MB
```

Up 2 and down 4 means a scan must be twice as consistent as the noise to hold its
credit. The softer decay while a scan runs tolerates the interleaving that a real
B-tree walk gives. So a point read pays for no read-ahead, and a table scan escalates
after three pages. Neither is configured.

rivet also keeps one tracker for each page class, B-tree and overflow, because a scan
over rows with large payloads reads a leaf, then an overflow page, then the next leaf.
One tracker sees alternating jumps and never scores a scan.

`../spec/Prefetch.lean` proves the behaviours: a scan escalates in three pages, random
access never escalates, the score is capped, a gap inside the tolerance is still a scan,
and one tracker misses the interleaved scan that two trackers find.

## What is built

`native/storeplane/` holds a SQLite VFS whose files live in FoundationDB, and a program
that proves the property the decision rests on.

```
=== process A: write ===
wrote 3 rows to zone-atlantis.db, no local file
=== process B: read, no local file ===
ls: cannot access 'zone-atlantis.db': No such file or directory
  owner = machine-a
  seq = 200
  zone = atlantis
read 3 rows from zone-atlantis.db in a new process, nothing was copied
```

SQLite wrote the rows through the VFS, no file exists on the disk, and a different
process read them. A handoff copies nothing because there is nothing to copy.

The layout is one key for one 4 kB block:

```
("weft", "afile", name, block_index) -> block bytes
("weft", "asize", name)              -> file size
```

That is the simplest correct layout, not the target. It is not rivet's layout, so it
has none of the properties `../spec/Store.lean` proves: a write is a transaction for
each `xWrite` rather than one for each commit, there is no PIDX, and there is no
compaction. The next increment replaces it.

Locking is a no-op, because an actor is the single writer of its own store.

## Measured

Against a live FoundationDB in a container, 500 rows, beside SQLite on a local file. The
local file is the floor, not the target: it is one machine with no durability across
machines.

| op | local file/s | FoundationDB/s | ratio |
| --- | --- | --- | --- |
| insert, one commit each | 1631 | 404 | 4.0x |
| insert, one commit for all | 216516 | 36838 | 5.9x |
| point read | 1283776 | 1192302 | **1.1x** |
| scan | 9532525 | 9642829 | **1.0x** |

A read costs what a local read costs. A write pays for the network, which is the trade
the design takes.

### One pragma is worth 2266 times

The first measurement gave 526 point reads each second, which is 249 times slower than a
local file. SQLite reads page 1 to check the change counter when a read transaction
starts, and over a network database that check is a round trip for each query.

`PRAGMA locking_mode=EXCLUSIVE` tells SQLite that nothing else can change the file, so
it trusts its page cache and stops re-reading. Point reads went from 526 to 1192302 each
second, a gain of 2266 times, and a scan gained 39 times.

This is not a trick. An actor is the single writer of its own store, so the statement is
true. The lesson is that the cost was never the page layout. It was a round trip that
the page cache should have absorbed.

## Two writers lost data, silently

The locks in the VFS are no-ops, so two writers both believe they hold the write lock.
Run with two writers on one database, before the fence:

```
writer 1: wrote 300, refused 0
writer 2: wrote 300, refused 0
integrity_check: ok
```

Both reported success. `PRAGMA integrity_check` passed. Every row belonged to writer 2,
and writer 1's 300 rows were gone. Silent loss, no error, and a database that checks out
as healthy.

This matters because the single-writer invariant is not absolute. `Weft.Actors` says
Horde is CRDT-based and chooses availability, so during a partition each side may
briefly run its own instance.

### The fence

Opening a database raises a number, and a writer that holds an older number is refused.
The fence is read inside the write transaction, so FoundationDB rejects the commit if
the fence moved. rivet does the same, in `depot_client_types::is_head_fence_mismatch`.

With the fence:

```
writer 1: wrote 162, refused 138
writer 2: wrote 300, refused 0
```

The loss became a loud failure. That is the whole point: a store may refuse a write, and
it may not accept a write and drop it.

## What is not handled

- **A FoundationDB conflict is not retried.** A retryable error becomes
  `SQLITE_IOERR_WRITE`. `fdb_transaction_on_error` is the fix.
- **A read transaction can age out.** FoundationDB limits a transaction to five seconds,
  and a long scan through one transaction will fail.
- **A commit is not one transaction.** Each `xWrite` commits on its own, so a SQLite
  commit that dirties several pages is not atomic. This is what the rivet layout in
  `../spec/Store.lean` fixes, and it is the next increment.

## Next

1. Read the two implementations that already do this: `mvsqlite`, and rivet's
   `engine/packages/depot-client`. Do not design a third.
2. Decide whether to use `mvsqlite` or to write the VFS. Writing one is a C API against
   `libfdb_c`, which is why the plane is native.
3. Build the plane on the thread-per-core harness over iceoryx v1, per `Weft`.
4. Delete `Weft.Actor.Store.Replicated`, `.Replicator`, and `Weft.Actor.load_all` when the
   plane serves reads.

## What this blocks

`Weft.Actor.load_all` reads the whole actor into memory, so an actor is memory-sized
until this plane exists. Every limit above kilobytes waits on it.
