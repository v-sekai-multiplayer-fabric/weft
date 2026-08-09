# Benchmarks

First-pass performance of the control-plane paths. Numbers are from a single dev
machine with a local single-node FoundationDB, so treat them as *relative* signals,
not production SLAs. Reproduce with `mix run bench/<name>.exs`.

## Store backends (`bench/store.exs`)

| Op | ips | median | vs fastest |
| --- | --- | --- | --- |
| SQLite put (write-through) | 14.3 K | 70 µs | — |
| SQLite load_all (100 keys) | 9.1 K | 105 µs | 1.6x slower |
| FDB load_all (100 keys) | 2.2 K | 448 µs | 6.6x slower |
| FDB put (one txn) | 1.0 K | ~1006 µs | 14.4x slower |

**Takeaway.** Node-local SQLite writes cost ~70 µs; FoundationDB writes cost ~1 ms
— roughly **14× more per write**. That millisecond is the price of
node-independence (a committed, any-node-reachable transaction), and it is what
makes cross-machine handoff possible. Pick per actor mobility: hot single-node
actors → SQLite; migratable/clustered actors → FDB.

## Actor ops (`bench/actor.exs`)

| Op | ips | median |
| --- | --- | --- |
| get_or_create warm (registry hit) | 1.12 M | 0.76 µs |
| actor get (call + memory cache) | 575 K | 1.66 µs |
| actor put (call + write-through) | 13.6 K | 69 µs |
| get_or_create cold (spawn + open + restore) | 1.6 K | 611 µs |

**Takeaway.** Addressing and cached reads are sub- to low-microsecond — the BEAM is
a cheap control plane. Write latency is dominated by the store (matches the SQLite
put above), not by the actor/GenServer overhead. Cold actor start (~0.6 ms) is
spawn + SQLite open + state restore; scale-to-zero pays this on wake, so tune the
idle window against wake frequency.

## Data-plane ingestion (`bench/data_plane.exs`, `bench/data_plane_ring.exs`)

Two mechanisms for getting digested snapshots (8 entities each) into the BEAM:

| Mechanism | snapshots/sec |
| --- | --- |
| Per-message (naive: full `%Snapshot{}` copied into the mailbox) | 1.38 M |
| Shared ring (`:atomics` seqlock), 1 core | 2.85 M |
| Shared ring, 2 cores | 5.68 M |
| Shared ring, 4 cores | 11.2 M |
| Shared ring, 8 cores | **21.6 M** |
| Shared ring, 16 cores | **27.7 M** |
| Ring sample (read) cost | ~3 µs/read |

**Takeaway.** Passing one Erlang message per snapshot copies a full term into the
mailbox and caps out around 1.4M/s — the wrong tool at this rate. The
contract-2 mechanism from `docs/data-plane.md` is a lock-free shared slot
(`Weft.DataPlane.Ring`, backed by `:atomics`): the worker overwrites it, the BEAM
samples it. That alone doubles single-core throughput (no copy, no mailbox), and
because each zone has its own ring it scales across cores: **>15M snapshots/sec is
reached at 8 cores (21.6M), and 27.7M at 16.** Sampling costs ~3 µs, so reading at
60 Hz is free.

The real producers are the C++ Seastar/iceoryx/Jolt workers writing the same ring
through a NIF (faster than an Elixir producer), and the BEAM only samples the
latest — so the raw 15M+ pps packet flood never enters the VM, and the digested
snapshot rate is not BEAM-bound either.
