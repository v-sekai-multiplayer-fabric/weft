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

## Data-plane ingestion ceiling (`bench/data_plane.exs`)

Flooding a single zone with digested snapshots (8 entities each) and measuring the
drain rate:

| Metric | Value |
| --- | --- |
| snapshots/sec into one zone | ~1.38 M |

**Takeaway.** The BEAM absorbs ~1.4M digested snapshots/sec **per zone**. At 60 Hz
that is headroom for ~23,000 zones per core-equivalent of digestion. This confirms
the boundary in `docs/data-plane.md`: the control plane handles digested state
comfortably, so the raw packet flood (the 15M+ pps path) stays in the C++ data
plane and never enters the BEAM.
