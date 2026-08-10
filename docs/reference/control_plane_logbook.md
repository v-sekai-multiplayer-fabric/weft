# Control plane logbook

Every measurement of the Elixir control plane, with the conditions it ran under.

A number without its conditions is not a result. The first entry below measured a store
that weft does not use, and the number was wrong by 288 times. So each entry names the
apparatus, the method, and the outcome. An entry that turned out to be invalid stays
here, and it says why.

The oldest entry is at the top, and a new entry goes at the bottom. This is the order of
a laboratory notebook. An entry records what happened at a time, and a later entry refers
to an earlier one. So the order must not change after the fact.

## Apparatus

Unless an entry says otherwise:

- One developer machine, with a single node FoundationDB beside it.
- Run with `mix run test/bench/<name>.exs`.
- The `stress bench` workflow gathers these on CI, as the `benchmarks-elixir` artifact.

Two hosts appear below, and the difference between them is load bearing.

- **Container on Windows.** An fsync is expensive here.
- **Linux CI runner.** An fsync is cheap here.

## Invalid: the store backends

`test/bench/store.exs`.

| operation | ips | median | against the fastest |
| --- | --- | --- | --- |
| SQLite put, write through | 14.3 K | 70 us | — |
| SQLite load_all, 100 keys | 9.1 K | 105 us | 1.6x |
| FDB load_all, 100 keys | 2.2 K | 448 us | 6.6x |
| FDB put, one transaction | 1.0 K | about 1006 us | 14.4x |

**Invalid. Do not cite it.** It measured `Store.Sqlite`, which uses the rollback journal
and fsyncs on each commit. That is not the store the design turns on. The store weft uses
is `Store.Replicated`, which uses WAL with `synchronous=NORMAL` and does not fsync on
each commit. It had never been measured at all.

The lesson is not that the numbers were slow. It is that nobody had asked which store the
bench opened.

## The store that weft uses

A Linux container against a live FoundationDB, on a Ryzen 7 3800X.

| operation | median | against the replicated put |
| --- | --- | --- |
| replicated put, WAL, no fsync | 19.2 us | — |
| replicated load_all, 100 keys | 144.6 us | 7.5x |
| sqlite load_all, 100 keys | 203.9 us | 10.6x |
| fdb load_all, 100 keys | 824.3 us | 43x |
| fdb put, one transaction | 1896.0 us | 99x |
| sqlite put, rollback journal, fsync | 5546.0 us | 288x |

The write path of the store in use costs 19 us, and not the 70 us the entry above
reported. Reads improved as well: 145 us against 204 us for 100 keys.

An fsync is the whole difference. The rollback journal fsyncs on each commit and costs
5.5 ms on this host. A synchronous FoundationDB write costs 1.9 ms. So the local store
that fsyncs is slower than the database over the network. That inverts what a reader
would assume.

That multiple does not travel. On the Linux CI runner the same rollback journal write
costs 70 us. The order of the rows holds on both hosts. The multiples do not.

The write path has a long tail. The median is 19.2 us and the 99th percentile is 181 us,
with a deviation of 542 percent. The tail is the replication cast and the compaction,
which share the scheduler of the caller.

## Actor operations

`test/bench/actor.exs`.

| operation | ips | median |
| --- | --- | --- |
| get_or_create warm, a registry hit | 1.12 M | 0.76 us |
| actor get, a call and the memory cache | 575 K | 1.66 us |
| actor put, a call and the write through | 13.6 K | 69 us |
| get_or_create cold, spawn and open and restore | 1.6 K | 611 us |

Addressing and a cached read cost less than a microsecond, or near it. A write costs what
the store costs, and not what the actor costs. A cold start costs about 0.6 ms, which is
the spawn, the SQLite open, and the restore of the state.

## Not measured

- **Many nodes.** Every number here is one node. Horde addressing across a cluster is not
  in these benches.
- **A cold actor under load.** The cold start above is one actor at a time on an idle
  machine.
