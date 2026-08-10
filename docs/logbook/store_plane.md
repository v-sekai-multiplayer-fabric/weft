# Store plane logbook

Every measurement of the store plane, with the conditions it ran under.

A number without its conditions is not a result. The same commit path reads 561 each
second on one cluster and 233 on another. So each entry below names the apparatus, the
method, and the outcome. An entry that turned out to be invalid stays here, and it says
why. A run that is deleted teaches nothing twice.

## Apparatus

Unless an entry says otherwise:

- Host: one machine, 16 cores, Linux.
- FoundationDB 7.3.76 in a container, reached over the container network.
- SQLite 3.53.4. `PRAGMA journal_mode=MEMORY` on every connection.
- The store plane programs are in `fabric-store-plane/`, built by CMake at `-O2`.

Two cluster shapes appear below. Name the shape in every new entry.

- **A**: one `fdbserver` process, `single memory`. One process holds every role.
- **B**: seven `fdbserver` processes, `single ssd`, with the classes split into one
  stateless, one log, and three storage. `deploy/compose.yaml` builds this shape.

Shape A is not a shape to run. It is recorded because the first measurements used it.

## 2026-08-09: one pragma is worth 2266 times

Shape A, `bench_vfs`, on the block layout that came before the one in place now.

The first measurement gave 526 point reads each second, which is 249 times slower than a
local file. SQLite reads page 1 to check the change counter when a read transaction
starts. Over a database on the network that check is a round trip for every query.

`PRAGMA locking_mode=EXCLUSIVE` tells SQLite that nothing else can change the file. It
then trusts its page cache and stops the re-read.

| setting | point reads/s | scan |
| --- | --- | --- |
| default | 526 | — |
| `locking_mode=EXCLUSIVE` | 1192302 | 39 times faster |

The gain is 2266 times, and it is not a trick. An actor is the single writer of its own
store, so the statement the pragma makes is true. The cost was never the page layout. It
was a round trip that the page cache had to absorb.

## 2026-08-09: the commit tears

Shape A. `prove_crash` over eight wall clock delays, 400 rows, against the layout that
gave one transaction to each `xWrite`.

Five of the eight left a torn database. Every one of the eight reported
`integrity_check: ok`. This is the fault behind issue 42, and the check that cannot see it.

## 2026-08-09: the search finds it

Shape A. `witness/` against the same layout, with the crash hook added to it.

A witness at the first rung, after nine crashes. The database it left could not be opened.

## 2026-08-09: the search does not find it after the fix

Shape A. `witness/` against the layout with one transaction for each commit.

- 240 candidates, crash points 1 to 60 at four commit sizes. None.
- 80 candidates, crash points 1 to 80 at one commit size. None.

## 2026-08-09: invalid, 2500 candidates

Shape A. `witness/` with 2500 candidates.

**Invalid. Do not cite it.** `prove_crash` was rebuilt while the search ran, so the run
tested two builds and neither completely. The run was stopped.

The lesson is that the thing under test must not change while a test runs. A search that
takes twenty minutes needs its binary held still for twenty minutes.

## 2026-08-09: 1100 candidates, after the round trips came out

Shape A. `witness/` against the current layout.

1100 candidates, crash points 1 to 220, at commit sizes of 1, 8, 64, 400, and 2000 rows.
Every crash point was in range. None left a torn database.

## 2026-08-09: the soak

Shape A. `soak.sh` for 90 seconds, 300 rows, 200 crash rows.

178 rounds of load, SIGKILL, and crash point rounds. No failures of any of the four kinds.

## 2026-08-09: against SQLite on a local file

Shape A, 500 rows, `bench_vfs`. Both sides run with the journal in memory, so neither
number holds an fsync. The local file is a reference and not a floor. It is one machine,
and it has no durability across machines.

| operation | local file/s | FoundationDB/s | ratio |
| --- | --- | --- | --- |
| insert, one commit each | 269105 | 561 | 480x |
| insert, one commit for all | 607002 | 80450 | 7.5x |
| point read | 2141392 | 2026893 | 1.1x |
| scan | 13946612 | 13350778 | 1.0x |

A read costs what a local read costs. A write pays for the network, and that is the trade
the design takes.

## 2026-08-09: what a commit costs

Shape A, 500 rows, `bench_vfs`.

| the commit path                               | commits/s |
| --------------------------------------------- | --------- |
| one transaction for each `xWrite`, not atomic | 404       |
| staged, two transactions                      | 280       |
| one transaction                               | 420       |
| one transaction, three reads removed          | 561       |

## 2026-08-09: the FoundationDB floor

Shape A, direct `libfdb_c`, no SQLite.

| what                         | commits/s | us each |
| ---------------------------- | --------- | ------- |
| one commit, 1 key of 64 B    | 928       | 1078    |
| one commit, 1 page of 4 kB   | 924       | 1082    |
| one commit, 8 pages of 4 kB  | 926       | 1080    |
| one commit, 64 pages of 4 kB | 545       | 1835    |

A commit is a round trip. The payload is nearly free until it is large.

## 2026-08-09: commits in flight, and handles

Shape A, one process, one commit of one key.

| in flight | commits/s |
| --------- | --------- |
| 1         | 928       |
| 2         | 1902      |
| 8         | 7733      |
| 32        | 19162     |
| 128       | 40993     |

Over several database handles, at 32 in flight: 19024 over two, 15648 over four, and 18500
over eight. A handle buys nothing. One client process has one network thread, and every
handle shares it.

## 2026-08-09: client processes

Shape A, but with the `ssd` engine, after the memory engine failed.

| processes and depth | in flight | commits/s |
| ------------------- | --------- | --------- |
| 1 x 32              | 32        | 18490     |
| 1 x 128             | 128       | 42212     |
| 2 x 64              | 128       | 45452     |
| 4 x 32              | 128       | 47816     |
| 8 x 16              | 128       | 49208     |
| 4 x 64              | 256       | 62949     |

At the same number in flight, eight processes beat one by about 17 percent. The variable
that matters is the number in flight, and not the number of processes.

## 2026-08-09: cluster shape

Shape B, the same client tests, on the same machine.

| processes and depth | in flight | commits/s |
| ------------------- | --------- | --------- |
| 1 x 128             | 128       | 35402     |
| 8 x 16              | 128       | 38850     |
| 4 x 64              | 256       | 52829     |
| 8 x 64              | 512       | 78437     |

One commit at a time costs 1585 to 2140 us, against 1078 us on shape A. `bench_vfs` reads
233 commits each second, against 561 on shape A.

More processes raise the ceiling under load. They lower the rate of one commit at a time,
because a commit crosses more processes. Both clusters ran on one machine with 16 cores,
beside the client processes, so both numbers include that contention.

## 2026-08-09: where the concurrency stops paying

Shape B. One client process, one commit of one key, five seconds for each depth. The
depth is the number of commits in flight. Two runs, to check that the shape is stable.

| depth | commits/s | mean us | max us | commits/s for each unit of depth |
| ----- | --------- | ------- | ------ | -------------------------------- |
| 1     | 713       | 1396    | 6126   | 713                              |
| 2     | 1195      | 1593    | 4417   | 598                              |
| 4     | 1968      | 1912    | 4052   | 492                              |
| 8     | 3625      | 2118    | 6125   | 453                              |
| 16    | 7044      | 2211    | 5378   | 440                              |
| 32    | 12918     | 2410    | 21025  | 404                              |
| 64    | 22889     | 2697    | 5928   | 358                              |
| 128   | 32981     | 3669    | 7805   | 258                              |
| 256   | 42525     | 5429    | 11927  | 166                              |
| 512   | 49368     | 9191    | 19260  | 96                               |

The second run gave 727, 1952, 7059, and 12958 at the depths 1, 4, 16, and 32. It gave
23824, 37102, 49192, and 56286 at the depths 64, 128, 256, and 512. The shape repeats.

### The pattern

The law of Little holds here. The number in flight is the rate times the latency. The
measured rate matches that product to within 3 percent up to depth 64. The error grows to
13 percent at depth 512. The cause is this program, which awaits in order, so a late
commit hides an early one.

So the curve has two parts, and the latency says which part it is in.

- **Up to about 64 in flight, the latency is nearly flat.** It moves from 1396 to 2697 us.
  That is 1.9 times. The rate over the same range rises 32 times. Each commit waits on
  the network, and not on the other commits.
- **Above that, the latency carries the load.** From 64 to 512 the rate rises 2.2 times.
  The latency rises 3.4 times. Depth stops to buy throughput, and it starts to buy queue.

The knee is near 64. The rate for each unit of depth falls slowly to that point, from 713
to 358. After the knee it falls fast, and it reaches 96 at depth 512.

### What it means for weft

`Weft.Limits` gives 32 in-flight requests. That value sits inside the flat
part of this curve. There an actor gets 12918 commits each second, at 1.7 times the
unloaded latency. The measurement supports the limit that was already written down.

The store plane keeps one commit in flight for one actor, because SQLite waits inside
`xSync`. So an actor alone sits at depth 1, which is the worst point on this curve. Depth
comes from the number of actors that commit at once, and not from any one of them.

## 2026-08-09: the memory engine stops the cluster

Shape A. Not a measurement. An incident, recorded because the symptom hides the cause.

The soak and the crash point searches wrote past the limit of the memory storage engine.
Every read and every write then hung, and `fdbcli` timed out. So the usual way to ask what
is wrong did not answer. Full `status` gave the cause:

```
Performance limited by server aed4c46cb0ff5258: Storage server running out of space (approaching 100MB limit).
```

The 100 MB in that line is not the cap. It is the headroom that FoundationDB warns about.
The cap is `--storage-memory`, and it defaults to 1 GiB for each process. The host has 128
GB of memory, and none of it raises that cap. The cap is a parameter, and it is not a
measure of the machine.

So there were two ways out. Raise `--storage-memory`, which the container image does not
expose. Or hold the data on disk, which the container already has.

`deploy/compose.yaml` now configures `ssd`. An existing cluster moves with
`configure storage_migration_type=aggressive ssd`.

## Not measured

- **Several machines.** Every number above is one machine, so the network is the container
  bridge and not a real link. `test/bench/fly` is the pattern for a real network test.
- **A cluster with cores to spare.** Shape B ran seven server processes on 16 cores. It ran
  up to eight client processes beside them. A larger machine separates the cost of the
  shape from the cost of the contention.
- **Read-ahead.** The read numbers come from a warm page cache. A cold scan pays a round
  trip for each page miss, and `../spec/Prefetch.lean` models what to do about it.

## 2026-08-10: the store plane against the 15 M target

No measurement here. Arithmetic on the runs above, written down because the question keeps
coming back and the answer is not close.

The data plane target is 15 M snapshots each second on one core.

| the store plane, from above | commits/s | short of 15 M by |
| --- | --- | --- |
| one commit at a time, through the VFS | 561 | 26738x |
| 32 in flight | 12918 | 1161x |
| 512 in flight | 49368 | 304x |

A batch closes some of it and not all of it. A commit carrying entities is one commit
whatever it carries, so multiply.

| entities in a commit | 32 in flight | 512 in flight |
| --- | --- | --- |
| 1 | 0.01 M | 0.05 M |
| 128 | 1.65 M | 6.32 M |
| 336 | 4.34 M | 16.59 M |

Only the last cell clears, and it is not a real operating point. 512 in flight costs 9191
us of mean latency and 19260 us at the worst, against 2410 us at 32. `Weft.Limits` holds 32
for exactly that reason.

### The wire format moves it, and it was not counted above

`../logbook/data_plane.md` measures two formats. The nasty bitpacked one is 12 bytes for
each entity. The cheap CBOR JSON-LD one is 28. That is 2.3 times, and it is 2.3 times the
entities in a commit of a given size.

The floor run above says the payload is nearly free until it is large: 924 commits each
second at 4 kB, 926 at 32 kB, and 545 at 256 kB. So a commit of 32 kB costs what a commit
of one key costs.

Put those together and the picture changes.

| format | commit size | entities in it | M snapshots/s at depth 1 | at depth 32 |
| --- | --- | --- | --- | --- |
| nasty, 12 B | 4 kB | 341 | 0.32 | 4.39 |
| nasty, 12 B | 32 kB | 2730 | 2.53 | 35.19 |
| cheap, 28 B | 4 kB | 146 | 0.13 | 1.88 |
| cheap, 28 B | 32 kB | 1170 | 1.08 | 15.08 |

**The depth 32 column is extrapolated and not measured.** The in-flight sweep used commits
of one key, and the payload sweep used one commit at a time. Nothing ran both. The column
multiplies the two, which assumes they compose, and a real run may not.

So the honest statement is that the format decides whether this is close or not close, and
that one bench would settle it: commits each second at 32 in flight with a 32 kB payload.
That bench does not exist.

**Either way the store plane is not the tick, and it is not supposed to be.** The floor is
1080 us for one commit, which is a network round trip. No amount of pipelining removes a
round trip, so this is a physical limit and not a tuning problem. The rows above are
aggregate throughput at 2410 us of latency, and a tick has a 66 ns budget for each
snapshot.

`CLAUDE.md` already states the rule this implies: keep durability and replication off the
write path.

### What that decides about deployment

If the only path between two machines is the store plane, then no path between two machines
carries per-tick state. 1161 times short is not a gap to engineer across.

So one world runs in one machine, and the planes of that world talk over iceoryx2.
`../essays/topology.md` already says this, and now there is a number behind it. The store
plane carries what survives a crash and what moves an actor to another machine. It does not
carry the tick.

The format still matters for the store, even so. 2.3 times the bytes is 2.3 times the
commits for the same state, and a commit is a round trip. So the choice between the two
formats is not only a bandwidth choice.
