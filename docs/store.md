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

| Aspect                     | rivet                            | weft                                          |
| -------------------------- | -------------------------------- | --------------------------------------------- |
| Local file                 | none                             | yes, the fast primary                         |
| Commit                     | sync FoundationDB txn (~1 ms)    | local (~µs), replicate async                  |
| Durability                 | strong                           | eventual                                      |
| Read                       | per page from FoundationDB       | local file; hydrate once on open              |
| FoundationDB role          | source of truth, on the hot path | durable replica and handoff, off the hot path |
| DELTA / SHARD / compaction | yes                              | yes, for the replica                          |
| Single writer              | Pegboard                         | Horde                                         |

## The store is a plane

The store runs SQLite natively in its own process, a store plane, the same as rivet
runs per-actor SQLite in rivetkit-core rather than in the orchestrator. The BEAM
control plane reaches it over iceoryx. The reason is crash isolation: SQLite in a
BEAM NIF takes the whole VM down if it faults, while a separate store plane can crash
and be restarted on its own (`planes.md`, why not a dirty NIF).

The store plane runs weft's low-latency design, not rivet's synchronous VFS to
FoundationDB: a local SQLite WAL file is the fast primary write path, and replication
to FoundationDB is asynchronous, off the write path. Latency is the priority, so the
per-write FoundationDB cost never sits on the path.

The logic is prototyped in Elixir today (`Weft.Actor.Store.Replicated` plus
`Weft.Actor.Store.Replicator`), tested against a live FoundationDB, so the design is
proven before the native port. The production store plane ports this same logic to a
native process behind iceoryx.

The boundary still holds: this store holds control-plane actor KV only, not game or
entity or world state. That is the data plane (`data-plane.md`); its durable form is
the asset CDN and the FoundationDB rows the data plane writes, not this store.

## Build order

1. Local SQLite store per actor in WAL mode, as the one store.
2. WAL tailer that ships frames to FoundationDB as DELTA rows.
3. Hydrate on open from FoundationDB (SHARD plus DELTA).
4. Background compaction (DELTA to SHARD).

## The S3-compatible cold tier

The store tiers in three levels. The write path is SQLite. The durable replica is
FoundationDB. The cold tier below FoundationDB is S3-compatible object storage. So the
full path is SQLite to FoundationDB to S3.

FoundationDB reaches the cold tier through its own backup. `fdbbackup` writes to an
S3-compatible endpoint with a `blobstore://` URL. This is a native FoundationDB
feature, so weft adds no custom backup code.

The S3 endpoint is [versitygw](https://github.com/versity/versitygw). versitygw is a
Go gateway that turns a local directory into an S3 server. One command runs it:
`versitygw --port :7070 posix /data`. The root credentials come from the
`ROOT_ACCESS_KEY` and `ROOT_SECRET_KEY` environment variables. It builds on Linux and
Windows.

So the cold tier is FoundationDB backups, written by `fdbbackup` to versitygw's S3
endpoint, stored on a plain directory. The deploy wires this. `deploy/compose.yaml`
runs versitygw plus a continuous `backup_agent` and `fdbbackup start`. The Quadlet unit
`deploy/quadlet/versitygw.container` runs the same gateway on Fedora.

The backup destination URL is:

```
blobstore://weft:weftsecret@versitygw:7070/fdb?bucket=fdb-backup&secure_connection=0&region=us-east-1
```

This keeps the tiering simple. SQLite serves the write path. FoundationDB serves the
durable replica. versitygw serves the cold S3 tier, with no cloud dependency.
