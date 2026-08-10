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
and fsyncs on each commit. The design at the time turned on `Store.Replicated`, which used
WAL with `synchronous=NORMAL` and skipped the fsync, and nobody had measured it at all.

The lesson is that nobody had asked which store the bench opened.

**Later, 2026-08-10.** `Store.Replicated` is deleted. Its compaction folded in place, which
`../spec/Store.lean` proves loses a page, and a CI run caught a read returning 200 and then
84. The real implementation is `fabric-store-plane`, and every actor here uses
`Store.Sqlite`. So the row above measures the store weft actually runs, and the entry stays
invalid for the original reason rather than this one.

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

## 2026-08-10: how long a Horde name stays invisible after it registers

One machine, 16 cores, `mix run` against the started application. Each round starts a zone
with `Weft.Zone.start_link`, which registers through
`{:via, Horde.Registry, {Weft.Registry, {:zone, id}}}`, and then looks the name up.

| shape | lookups that found nothing |
| --- | --- |
| sequential, same process, 200 rounds | 0 |
| sequential, lookup in another process, 200 rounds | 0 |
| sequential, 600 rounds, one worker | 0 |
| **32 concurrent workers, 20 rounds each** | **631 of 640** |

Every one of the 631 appeared later. Polling at 1 ms until it did:

| | microseconds |
| --- | --- |
| minimum | 1 |
| median | 1586 |
| maximum | 2048 |

So a register that returned is invisible to a lookup for about 2 ms, and only under
concurrent registration.

### Why

`Horde.Registry.register` writes to the delta CRDT. `Horde.RegistryImpl.on_diffs/2` then
does `Kernel.send(name, {:crdt_update, diffs})`, and the registry process materialises the
name into ETS when it handles that message. `lookup` reads that ETS table.

So the visible name is behind one process mailbox. Sequentially the mailbox is empty and
the gap is unmeasurable. At 32 registrations at once it is the queue.

This is how Horde works. It is not a setting, and the 300 ms `sync_interval` in
`delta_crdt_options` is a different mechanism that does not explain a 2 ms number.

### What it broke, and what it still means

`Weft.GatewayTest` "routes to a zone" failed about one run in three at seed 1, and never
alone. It starts a zone and dispatches to it, and `Weft.Gateway.dispatch/1` resolves a zone
with `Horde.Registry.lookup`. Inside that 2 ms it returns `{:error, :no_zone}`.

The test waits for the name now, because the test is about routing.

**The gateway behaviour is unchanged and it is real.** A caller that starts a zone and
dispatches to it at once can be told there is no such zone. That is not a test artifact,
and it is not fixed here. What to do about it is a design question: retry inside
`dispatch`, make the caller retry, or address a zone without asking the registry.
