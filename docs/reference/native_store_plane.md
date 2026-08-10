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

State: the VFS is built, and it holds the layout below. A commit is one FoundationDB
transaction. Reads, writes, compaction, and the fence run against a live cluster. The
plane itself is not built. There is no iceoryx harness and no thread-per-core loop yet,
so nothing calls this VFS except the programs in `native/storeplane/`.

The Elixir prototype, `Weft.Actor.Store.Replicated` and `.Replicator`, still exists. It
is not this design. It uses SQLite as a key and value table, it replicates logical rows
rather than pages, and `Weft.Actor.load_all` reads the whole actor into memory when the
actor starts.

## Why a VFS

A VFS gives SQLite its pages one page at a time. So SQLite reads the pages a query
touches and no others. Three things follow, and none of them is true of the prototype.

- **An actor is not limited by memory.** The working set is in memory, and the rest is in
  FoundationDB. This is what makes the 10 GiB limit in `Weft.Limits` possible.
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

The layout is the one above, and the keys are these:

```
weft/db/<name>/HEAD                 the txid of the newest commit
weft/db/<name>/SIZE                 the file size
weft/db/<name>/FENCE                the ownership fence
weft/db/<name>/PIDX/<pgno>          the txid that owns a page
weft/db/<name>/DELTA/<txid>/<pgno>  the pages of one commit
weft/db/<name>/SHARD/<as_of>/<pgno> a compacted base, versioned
weft/db/<name>/SHARDN/<as_of>       the page count of a shard version
weft/db/<name>/LOGN                 the page count of the log since compaction
weft/db/<name>/PIN/<txid>           a read that holds a shard version
```

A read finds the owner in PIDX and then reads one of DELTA or SHARD. So a read touches
two rows whatever the log holds. `../spec/Store.lean` proves this as
`read_touches_two_rows`.

### A commit is one transaction

The VFS holds the pages SQLite writes in memory. It sends them when SQLite syncs the
file, which is the end of a commit. So the pages of one commit reach FoundationDB
together. A crash leaves the whole commit, or it leaves none of it.

A commit that fits one transaction is one transaction. That is the common case, and it
costs one round trip.

A commit too large for one transaction stages instead. The pages go first, under a txid
that no read can reach, and one more transaction then moves the head. This is the shape
CockroachDB calls a parallel commit. The staged pages are safe to leave behind, because
the next open clears every txid above the head.

### Compaction

Compaction folds the log into a new shard version. It adds a version and never
overwrites one. It clears a PIDX row only when that row points at a folded txid.
`../spec/Store.lean` proves that these two rules preserve every read.

The trigger is a ratio and not a number. Compaction runs when the log is as large as the
base. A ratio has no units to tune, and it moves with the load. A quiet actor never
compacts.

Retention follows demand. A shard version below the oldest pin is one that nobody can
read, so compaction drops it. Nothing writes a pin yet.

Locking is a no-op, because an actor is the single writer of its own store.

## Measured

Against a live FoundationDB in a container, 500 rows, beside SQLite on a local file. Both
run with the journal in memory, so neither number includes an fsync. The local file is a
reference and not a floor: it is one machine with no durability across machines.

The shape of the cluster changes every number below, so it is part of the measurement. The
table comes from a cluster of one process. That is not a shape to run, because FoundationDB
gives one role to one process, and a single process runs the commit proxy, the resolver,
the log, and the storage together. On a cluster of seven processes with the classes split,
on the same machine, the same commit path reads 233 each second rather than 561. A commit
crosses more processes, and this is one machine.

More processes raise the ceiling under load, and they lower the rate of one commit at a
time. The store plane commits one at a time and waits, so it takes the second cost and not
the first. A machine with cores to spare does not have this problem.

`store_plane_logbook.md` holds every measurement with the conditions it ran under. It also
holds the runs that turned out to be invalid, and why.

| op | local file/s | FoundationDB/s | ratio |
| --- | --- | --- | --- |
| insert, one commit each | 269105 | 561 | 480x |
| insert, one commit for all | 607002 | 80450 | 7.5x |
| point read | 2141392 | 2026893 | **1.1x** |
| scan | 13946612 | 13350778 | **1.0x** |

A read costs what a local read costs. A write pays for the network, which is the trade the
design takes.

The atomic commit is faster than the torn one it replaced.

| the commit path | commits/s |
| --- | --- |
| one transaction for each `xWrite`, not atomic | 404 |
| staged, two transactions | 280 |
| one transaction | 420 |
| one transaction, three reads removed | **561** |

### The write path is latency, not work

FoundationDB commits in about 1.1 ms whatever the commit carries. One key of 64 bytes and
eight pages of 4 kB both cost 1080 us. So a commit is a round trip, and the payload is
almost free until it is large.

That number is the ceiling for one writer that waits for each commit. The VFS reached 420
of a possible 928, because it made five round trips for each commit. It read the fence,
read the log count, committed, and then read two more counts to decide about compaction.

Three of those reads asked the database for numbers the writer already knew. A single
writer owns the log count and the base count, so the handle keeps both. What is left is
the fence read and the commit. The fence read has to stay: a writer that lost ownership
between two of its own commits conflicts with nothing, so nothing else would catch it.

### Where an order of magnitude is

Not in a faster client, and not in more connections to FoundationDB.

| | commits/s |
| --- | --- |
| one commit at a time | 928 |
| 2 in flight | 1902 |
| 8 in flight | 7733 |
| 32 in flight | 19162 |
| 128 in flight | **40993** |
| 32 in flight over 2 handles | 19024 |
| 32 in flight over 4 handles | 15648 |
| 32 in flight over 8 handles | 18500 |

Commits in flight are worth 44 times. More database handles are worth nothing, because one
client process has a single network thread and every handle shares it. The parallelism has
to come from transactions in flight.

One database cannot pipeline its own commits. SQLite waits inside `xSync` until the commit
returns, so the next commit has not been asked for yet. The parallelism has to come from
many actors committing at once inside one plane. That is what iceoryx is for. It is also why
the plane is one process that many actors reach, and not one process for each actor.

Reads have no such room. They are already at parity with a local file. The page cache
absorbs them, and no round trip happens at all.

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
writer 1: wrote 200, refused 0
writer 2: wrote 0, refused 200
```

The loss became a loud failure. That is the whole point: a store may refuse a write, and
it may not accept a write and drop it.

The fence guards every write transaction, and not only the commit. The first version
checked it in the commit alone, so a stale writer could still compact. Compaction drops
the shard version that the owner reads. The owner then read pages that were gone, and both
writers failed against a database that was intact. A fence that covers one write path and
not the others is not a fence.

## A crash point is a better test than a delay

`prove_crash` crashes a writer and then looks for a database that holds half of a commit.
`PRAGMA integrity_check` cannot see that fault, because a database that mixes pages from
two commits is structurally valid. So the program checks the contents directly: every
round writes the same text into every row, and two distinct values mean a torn commit.

It crashes in two ways. `kill` sends SIGKILL after a delay, which is the crash a machine
gives. `at` stops the writer before a numbered commit. The second repeats exactly, and a
search cannot address a failure it cannot repeat.

`witness/` searches the crash points with [plausible-witness-dag][pwd]. A candidate names
a commit size and a crash point. plausible samples the space at each rung of a ladder, and
a deterministic walk then covers it in order. The two negatives differ, and the difference
is the point: a budget hit means the search did not look everywhere, and `provablyNone`
means it did.

[pwd]: https://github.com/fire/plausible-witness-dag

Against the layout that committed each `xWrite` on its own, the search finds a witness at
the first rung after nine crashes. Against the layout above, it covers 1100 candidates and
finds none. Those are the crash points 1 to 220, at commit sizes of 1, 8, 64, 400, and 2000
rows.

## Every transaction retries

FoundationDB expects a client to retry. `fdb_transaction_on_error` decides whether an
error may be retried, and it waits the right amount before the next attempt. Every
transaction in the VFS runs in that loop.

This matters for two errors that arrive with load rather than with a bug. Error 1020,
`not_committed`, means the transaction conflicted. Error 1007, `transaction_too_old`,
ends a read that ran past the five second limit. Both were hard failures before, and
both now retry.

A fence mismatch does not retry. Refusing the write is the correct answer, so it reaches
the caller as `SQLITE_READONLY`.

## What is not handled

- **There is no read-ahead.** A page miss is a network round trip, and the section above
  says read-ahead is most of the engineering. `../spec/Prefetch.lean` models it. The VFS
  reads one page at a time.
- **Nothing writes a pin.** Compaction reads the pin range and keeps every version at or
  above the oldest pin. No reader creates one, so only the newest version survives, and
  a read below the head has nothing to hold.
- **A very large commit cannot be indexed.** The pages of a large commit stage across
  transactions. The index rows must still fit one transaction. The limit comes from the
  FoundationDB transaction size, and it is not chosen.
- **One writer commits, not many.** The fence gives one owner at a time. Committing from
  several actors at once needs the parallel commit protocol, modelled in
  [ParallelCommits.tla][pc]. That is a different problem from the one the fence solves.

[pc]: https://github.com/V-Sekai/cockroach/blob/release-22.1-v-sekai/docs/tla-plus/ParallelCommits/ParallelCommits.tla

## Next

1. Add read-ahead, which `../spec/Prefetch.lean` already models. This is the largest
   remaining piece, and it is what makes a scan affordable.
2. Build the plane on the thread-per-core harness over iceoryx v1, per `Weft`. Nothing
   calls the VFS yet except the programs in `native/storeplane/`.
3. Write a pin when a read needs a version below the head, so a restore point survives
   compaction.
4. Delete `Weft.Actor.Store.Replicated`, `.Replicator`, and `Weft.Actor.load_all` when the
   plane serves reads.

## What this blocks

`Weft.Actor.load_all` reads the whole actor into memory, so an actor is memory-sized
until this plane exists. Every limit above kilobytes waits on it.
