# Store

The store holds actor state (KV and SQLite). It must be low latency (see
`latency.md`). This page reviews how rivet stores SQLite in FoundationDB, then gives
weft's design.

## How rivet does it

rivet keeps no local SQLite file. All state lives in FoundationDB. A custom SQLite
VFS turns page reads and commits into FoundationDB work:

- **Commit.** The VFS hands dirty pages to a conveyer. The conveyer encodes them as
  DELTA chunks, writes a page index (PIDX: page number to owning DELTA), writes the
  commit records, and advances the head — all in one FoundationDB transaction. The
  SQLite commit waits for that transaction. A commit costs one FoundationDB
  transaction, about 1 ms.
- **Read.** For each page, the conveyer walks branch history, uses PIDX to find the
  latest DELTA, and falls back to a compacted SHARD.
- **Shape.** This is an LSM. DELTAs are L0 (recent writes). SHARDs are L1 (compacted
  base). A background workflow folds DELTAs into SHARDs.
- **Why.** No local file means state can move between machines. A commit is durable
  once FoundationDB commits.

rivet's choice: strong durability, about 1 ms per commit.

## weft's requirement

Low latency first. About 1 ms per commit on the write path is too slow. weft accepts
weaker durability to get fast writes.

## weft's design

One store design for every actor:

- **Local SQLite file per actor, in WAL mode.** Commits are local and fast (about a
  microsecond). This is the write path.
- **Async replication to FoundationDB.** A background task tails the WAL and ships
  new frames to FoundationDB, off the commit path. A commit never waits for
  FoundationDB.
- **FoundationDB storage.** The same LSM shape as rivet: DELTA rows for shipped WAL
  frames, a compacted SHARD base, and a background compaction. The writer is the
  replicator, not the commit.
- **Hydrate on open.** When an actor opens on a machine with no local file (first
  start, or after handoff to a new machine), it rebuilds the local file once from the
  latest SHARD plus later DELTAs, then serves all reads from the local file. This is
  rivet's read path, done once at open, not per read.
- **Compaction.** A background task folds DELTA rows into a new SHARD, like rivet.
- **Single writer.** Horde gives one actor process per id, the same guarantee rivet
  gets from Pegboard.

Accepted cost: a crash can lose the last few commits that were not yet replicated.
This is the trade for low-latency writes.

## rivet vs weft

| Aspect | rivet | weft |
| --- | --- | --- |
| Local file | none | yes, the fast primary |
| Commit | sync FoundationDB txn (~1 ms) | local (~µs), replicate async |
| Durability | strong | eventual |
| Read | per page from FoundationDB | local file; hydrate once on open |
| FoundationDB role | source of truth, on the hot path | durable replica and handoff, off the hot path |
| DELTA / SHARD / compaction | yes | yes, for the replica |
| Single writer | Pegboard | Horde |

## Build order

1. Local SQLite store per actor in WAL mode, as the one store.
2. WAL tailer that ships frames to FoundationDB as DELTA rows.
3. Hydrate on open from FoundationDB (SHARD plus DELTA).
4. Background compaction (DELTA to SHARD).
