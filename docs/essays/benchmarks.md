# Benchmarks

First-pass performance of the control-plane paths. Numbers are from a single dev
machine with a local single-node FoundationDB, so treat them as _relative_ signals,
not production SLAs. Reproduce with `mix run test/bench/<name>.exs`.

The `stress bench` GitHub Actions workflow gathers these numbers on CI. It uploads the
native benches as the `benchmarks-native` artifact, the Elixir benches as
`benchmarks-elixir`, and the SUMO console recording as `stress-bench-cast` (play it
with `asciinema play`). Download the artifacts to update this page.

## Store backends (`test/bench/store.exs`)

| Op                         | ips    | median   | vs fastest   |
| -------------------------- | ------ | -------- | ------------ |
| SQLite put (write-through) | 14.3 K | 70 µs    | —            |
| SQLite load_all (100 keys) | 9.1 K  | 105 µs   | 1.6x slower  |
| FDB load_all (100 keys)    | 2.2 K  | 448 µs   | 6.6x slower  |
| FDB put (one txn)          | 1.0 K  | ~1006 µs | 14.4x slower |

### Correction: the row above measures the wrong store

`bench/store.exs` measured `Store.Sqlite`, which uses the **default rollback journal**
and fsyncs on each commit. That is not the store the design turns on. The replicated
store, `Store.Replicated`, uses WAL with `synchronous=NORMAL` and does not fsync on
each commit. It had never been measured.

Measured in a Linux container against a live FoundationDB, on a Ryzen 7 3800X:

| Op | median | vs replicated put |
| --- | --- | --- |
| **replicated put (WAL, no fsync)** | **19.2 µs** | — |
| replicated load_all (100 keys) | 144.6 µs | 7.5x |
| sqlite load_all (100 keys) | 203.9 µs | 10.6x |
| fdb load_all (100 keys) | 824.3 µs | 43x |
| fdb put (one txn) | 1896.0 µs | 99x |
| sqlite put (rollback journal, fsync) | 5546.0 µs | **288x** |

Three things fall out.

**The write path is 288 times cheaper than the number the page reported.** The store
weft actually uses writes in 19 µs, not 70 µs, and the 70 µs figure came from a store
with different pragmas.

**Reads got faster too.** The replicated store reads 100 keys in 145 µs against the
rollback-journal store's 204 µs. WAL wins on both paths here, so the write bias costs
nothing on reads.

**fsync is the whole difference.** The rollback journal fsyncs on each commit and costs
5.5 ms in this container. A synchronous FoundationDB write costs 1.9 ms, so *the
fsyncing local store is slower than the network database*. That inverts the usual
assumption, and it is why the pragmas matter more than the backend.

One caveat on portability: this container runs on a Windows host, where fsync is
expensive. On the Linux CI runner the same rollback-journal write costs 70 µs. The
ordering holds in both, but the multiples do not transfer. What does transfer is that
the measured store was never the store in use.

The write path also has a long tail: 19.2 µs median against a 181 µs 99th percentile,
and a 542 percent deviation. The tail is the replication cast and the compaction
sharing the caller's scheduler. See `Weft.Actor.Store`.

**Takeaway.** Local SQLite writes cost ~70 µs; a synchronous FoundationDB write
costs ~1 ms, about **14× more**. Latency is the priority (see `latency.md`), so the
write path is the local fast store, and durability plus cross-machine handoff run
asynchronously, off the write path, replicating to FoundationDB. The 1 ms
FoundationDB write is never on the critical path. This is one store design for every
actor (local primary, async FoundationDB replica), not a per-actor choice.

## Actor ops (`test/bench/actor.exs`)

| Op                                          | ips    | median  |
| ------------------------------------------- | ------ | ------- |
| get_or_create warm (registry hit)           | 1.12 M | 0.76 µs |
| actor get (call + memory cache)             | 575 K  | 1.66 µs |
| actor put (call + write-through)            | 13.6 K | 69 µs   |
| get_or_create cold (spawn + open + restore) | 1.6 K  | 611 µs  |

**Takeaway.** Addressing and cached reads are sub- to low-microsecond — the BEAM is
a cheap control plane. Write latency is dominated by the store (matches the SQLite
put above), not by the actor/GenServer overhead. Cold actor start (~0.6 ms) is
spawn + SQLite open + state restore; scale-to-zero pays this on wake, so tune the
idle window against wake frequency.

## Data-plane ingestion (`test/bench/data_plane.exs`, `test/bench/data_plane_ring.exs`)

Two mechanisms for getting digested snapshots (8 entities each) into the BEAM:

| Mechanism                                                       | snapshots/sec |
| --------------------------------------------------------------- | ------------- |
| Per-message (naive: full `%Snapshot{}` copied into the mailbox) | 1.38 M        |
| Shared ring (`:atomics` seqlock), 1 core                        | 2.85 M        |
| Shared ring, 2 cores                                            | 5.68 M        |
| Shared ring, 4 cores                                            | 11.2 M        |
| Shared ring, 8 cores                                            | **21.6 M**    |
| Shared ring, 16 cores                                           | **27.7 M**    |
| Ring sample (read) cost                                         | ~3 µs/read    |

**Takeaway.** Passing one Erlang message per snapshot copies a full term into the
mailbox and caps out around 1.4M/s — the wrong tool at this rate. The
contract-2 mechanism from `Weft.DataPlane` is a lock-free shared slot
(`Weft.DataPlane.Ring`, backed by `:atomics`): the worker overwrites it, the BEAM
samples it. That alone doubles single-core throughput (no copy, no mailbox), and
because each zone has its own ring it scales across cores: **>15M snapshots/sec is
reached at 8 cores (21.6M), and 27.7M at 16.** Sampling costs ~3 µs, so reading at
60 Hz is free.

The real producers are the native C++ planes writing the same ring through a NIF
(faster than an Elixir producer), and the BEAM only samples the latest — so the raw
15M+ pps packet flood never enters the VM, and the digested snapshot rate is not
BEAM-bound either.

## SUMO plane — real workload, not synthetic (`test/bench/sumo/`)

The numbers above use synthetic packets. To confirm them on real, coherent movement,
weft acts as the engine for a SUMO (Eclipse traffic microsimulation) run: a 25 by 25
grid city, dense traffic, each vehicle an entity, each simulation step a state frame.
The trace is 600 frames, 11,947 distinct vehicles, 8,637 peak concurrent, 2,950,620
entity updates. See `test/bench/sumo/README.md` to reproduce and `Weft.Gateway` for
the full analysis.

Nasty hot-path decode plus apply (`test/bench/sumo/replay.c`), on the real trace:

| cores | pps    | ns/apply/core |
| ----- | ------ | ------------- |
| 1     | 840 M  | 1.19          |
| 8     | 6.19 B | 1.29          |
| 16    | 7.78 B | 2.06          |

**Takeaway.** 840M applies/sec on one core (1.19 ns each) on real traffic movement,
**56× the 15M target**, matching the synthetic `pps_native.c` (826M/core). Real data
confirms the synthetic ceiling: apply is never the bottleneck. The same trace drives
the cheap-versus-nasty wire-format comparison (`test/bench/sumo/encode_compare.py`): nasty
bitpacked is 12 B/entity, cheap CBOR JSON-LD is 28 B/entity (2.3× raw, 1.4× after
last-frame zstd). Details in `Weft.Gateway`.

## Packet decode+apply — is 15M pps compute- or I/O-bound? (`test/bench/pps_native.c`)

The true ">15M pps" unit is a _packet_: decode a 24-byte movement datagram and
apply it to an entity slab. Native, packets replayed from a cache-hot batch:

| cores | packets/sec | ns/pkt/core | ingress @24B |
| ----- | ----------- | ----------- | ------------ |
| 1     | 826 M       | 1.21        | 19.8 GB/s    |
| 4     | 2.56 B      | 1.57        | 61 GB/s      |
| 8     | 5.09 B      | 1.57        | 122 GB/s     |
| 16    | 9.32 B      | 1.72        | 224 GB/s     |

**Takeaway.** Decode+apply costs ~1.2 ns/packet — **826M pps on one core, ~55× the
15M target.** Compute is nowhere near the bottleneck; 15M pps needs ~2% of a single
core. What actually caps 15M pps is **moving the packets** (NIC → memory), which is
why the ladder is AF_XDP → DPDK → SmartNIC, not "faster decode logic." This is the
empirical basis for keeping the hot path native and kernel-bypass, and the BEAM out
of it entirely.

Caveat: this is the cache-hot compute ceiling (packets and hot entities in L1/L2).
Real traffic adds NIC DMA and scattered-entity cache misses, so per-packet cost
rises — the DRAM-bound number below is the honest version.

### DRAM-bound apply — the real ceiling for large worlds (`test/bench/pps_dram.c`)

Random writes into a 2 GB entity table (64M entities, far beyond the 32 MiB L3):

| cores | packets/sec | ns/op/core |
| ----- | ----------- | ---------- |
| 1     | 41.2 M      | 24.2       |
| 4     | 102.7 M     | 38.9       |
| 8     | 115.5 M     | 69.3       |
| 16    | 117.5 M     | 136.1      |

**Takeaway.** Random DRAM access is ~20× slower than cache-hot (41M vs 826M per
core), and the aggregate **plateaus at ~117M pps — the DRAM bandwidth wall** (8→16
cores barely moves). Two conclusions: (1) even pessimistically, one core clears the
15M target by ~2.7×, so **apply is never the bottleneck** — packet I/O is; (2) the
20× cache-vs-DRAM gap is why entity layout and area-of-interest locality (ECS-style
hot arrays, spatial partitioning) matter at extreme scale, and why the ~117M wall,
not compute, is the ultimate apply ceiling on this machine.

## State-replication bandwidth — last-frame zstd dictionary (`test/bench/zstd_frames.c`)

The other wall is bytes on the wire (server → client fanout, and cloud egress $).
A state frame is 256 entities × 20 B = 5 KB; consecutive frames differ only where
entities moved, so compressing frame _n_ with frame _n-1_ as the zstd dictionary
sends only the deltas. At 30% of entities moving per frame:

| Scheme                              | bytes/frame | ratio    | compress | decompress |
| ----------------------------------- | ----------- | -------- | -------- | ---------- |
| no dictionary (level 1)             | 5130        | 1.0×     | 0.3M f/s | —          |
| **last-frame dictionary (level 1)** | **787**     | **6.5×** | 0.1M f/s | 0.1M f/s   |

**Takeaway.** Using the previous frame as the dictionary cuts replication bandwidth
**~6.5× at 30% churn** (more for calmer scenes), at ~100K frames/s/core — plenty for
60 Hz fanout to thousands of clients. This multiplies effective throughput against
the bandwidth/egress wall and cuts cloud egress cost by the same factor. Level 1 ≈
level 3 on ratio here, so use level 1 for latency. Note this is a _replication_
optimization (large, coherent frames); it does nothing for the tiny, independent
24-byte input packets on the ingest side.

## Real-NIC packet I/O (`test/bench/fly/netbench.c`) — pending, cost-gated

Loopback cannot measure the receive ceiling. `netbench` (UDP server/client over
IPv6/6PN) is ready to run between two Fly machines for the real number, but is held
until a cost-bounded run (smallest shared-CPU machines, seconds of traffic, torn
down immediately). Compressed payloads (above) also minimize egress during the run.

### On measuring the I/O ceiling here (`test/bench/udp_recv.c`)

Attempting to measure the kernel receive ceiling on loopback gave ~0.16M pps with
**no gain from `recvmmsg` batching over `recv()`** — the tell that it is not
measuring the receive path but loopback send-side throttling (ENOBUFS / single
softirq core). Loopback UDP is not a valid proxy for NIC receive. A trustworthy
kernel-vs-AF_XDP receive comparison needs a real NIC (or an AF_XDP veth setup),
which this box does not have. The tool is kept for a NIC-equipped host; the
number here is not load-bearing. The decisive, clean numbers stand: compute is
826M pps/core and the I/O tax (which kernel bypass removes) must be measured on
hardware, not loopback.
