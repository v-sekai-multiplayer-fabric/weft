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
- The store plane programs are in `native/storeplane/`, built by CMake at `-O2`.

Two cluster shapes appear below. Name the shape in every new entry.

- **A**: one `fdbserver` process, `single memory`. One process holds every role.
- **B**: seven `fdbserver` processes, `single ssd`, with the classes split into one
  stateless, one log, and three storage. `deploy/compose.yaml` builds this shape.

Shape A is not a shape to run. It is recorded because the first measurements used it.

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
| --- | --- | --- | --- | --- |
| 1 | 713 | 1396 | 6126 | 713 |
| 2 | 1195 | 1593 | 4417 | 598 |
| 4 | 1968 | 1912 | 4052 | 492 |
| 8 | 3625 | 2118 | 6125 | 453 |
| 16 | 7044 | 2211 | 5378 | 440 |
| 32 | 12918 | 2410 | 21025 | 404 |
| 64 | 22889 | 2697 | 5928 | 358 |
| 128 | 32981 | 3669 | 7805 | 258 |
| 256 | 42525 | 5429 | 11927 | 166 |
| 512 | 49368 | 9191 | 19260 | 96 |

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

`actor_limits.md` already gives 32 in-flight requests. That value sits inside the flat
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
