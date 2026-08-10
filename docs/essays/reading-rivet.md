# What rivet already learned that weft has not

weft copies rivet's store layout, which `CLAUDE.md` says outright. So the interesting
question is not what rivet does. It is where rivet's own documents disagree with weft's,
because one of the two is wrong and it is cheaper to find out by reading than by shipping.

`docs-internal/engine/` holds 77 pages. Five things in them matter here.

## The one where weft's rule is already dead

rivet lists **no local SQLite files** as a binding constraint. The reason is one line:
"Local files would make storage stateful and non-migratable."

`CLAUDE.md` says the opposite. It says the store "tiers a local SQLite WAL primary to a
FoundationDB replica to S3-compatible object storage".

The code settled this before either document did. `fdb_vfs.c` opens with "no local file, so
an actor's database moves between machines with no copy and no restore", and it sets
`PRAGMA journal_mode=MEMORY` so SQLite has nowhere to put a journal.

So weft already built rivet's answer and kept describing the other one. That is the drift
`Weft.VocabularyTest` exists to catch, in a form no test could see: not a retired word, a
retired design.

Fixing it moved two essays as well. `../essays/latency.md` argues that durability belongs
off the write path, and the plane appears to break that: a commit is a FoundationDB
transaction at about 1080 microseconds.

It does not break it, and the reason is worth stating because it reads the wrong way at
first. The unit changed. A write inside a transaction touches the page cache and costs
nothing, and only the commit crosses the network. So the millisecond is paid once for each
transaction rather than once for each write.

What is genuinely on the path now is a page miss, at the same 1080 microseconds. That is a
different cost than the prototype had, and read-ahead is what pays it down.

## The engine owns rollback, and storage never sees it

This is the one worth taking next.

Cloudflare Durable Objects, Turso, and Neon all put rollback in storage. rivet does not.
Storage exposes fork, delete, and restore point, and the engine does this instead:

1. Resolve a restore point or an as-of versionstamp.
2. Fork the database.
3. Point the mapping at the new id.
4. Reconnect.

What that buys is stated plainly: no mutable pointer swaps, no pointer history, no frozen
states, and no race between a commit and a rollback. Storage keeps one rule, which is that
a branch id is immutable for life.

weft has no branches at all. `../spec/Store.lean` models PIDX, DELTA, and SHARD, and
nothing above them. So weft cannot fork, and it cannot roll back either. That is a gap and
not a mistake, because nothing has asked for it yet.

## Single writer, and what it lets you not build

rivet declines mvSQLite's multi-writer machinery, and gives the reason: pegboard already
guarantees a single writer for each database, so conflict resolution "would add cost
without buying correctness".

weft has the same guarantee under a different name. Authority is the single writer of an
entity, and the store plane fences on it. So the same refusal applies, and for the same
reason.

It is worth writing down because the pressure to add multi-writer support arrives disguised
as a feature request. The answer is that the fence already made it unnecessary.

## Pages that describe themselves

rivet's layers carry page numbers and checksums inside the page data. The consequence is
the interesting part: bytes move between a DELTA row and a SHARD row with no separate page
map to keep in step. PIDX stays a hot routing index and stops being a source of truth.

weft's compaction has to rewrite PIDX because a weft page does not say what it is. That is
a real difference in how much can go wrong during a fold, and
`../logbook/store_plane.md` records that weft has already had one fold bug.

## Placement rides a hash ring

`envoy-load-balancing.md` solves a problem weft has not reached: which host takes a new
actor. It is a virtual node ring keyed by xxh3, with a knob K.

At K=1 it short circuits and costs three reads. At K greater than 1 it reads each
candidate's slot count and takes the minimum, which is power of K choices. The reason given
for the ring is contention: reading the whole subspace on each allocation "pile-ups on the
FDB shards owning that range and degrades the whole system".

`Weft.Pool` solves the neighbouring problem, how many runners a pool should have, and it
solves it the same way in spirit. It recomputes desired from what is observed rather than
accumulating a counter, because rivet's own accumulator drifted negative and wedged the
pool at zero. Placement will want the same discipline.

## What this reading did not find

A throughput number for anything on rivet's hot path. Their published benchmarks are cold
start, at 4.8 ms for the median, and memory for each machine, between 22 and 131 MB.

That absence is informative rather than a gap in their documents. rivet's actors are
request and response workloads, so start time and footprint are the right things to
measure. weft's data plane is a tick, and a tick has 66 ns for each snapshot.

So rivet's shape is worth copying for the link, for placement, and for the store. It is not
worth copying for the tick, because rivet does not have one.
