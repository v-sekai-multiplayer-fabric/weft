# Data plane logbook

Every measurement of the data plane, with the conditions it ran under.

A number without its conditions is not a result. The same apply costs 1.2 ns on a cache
hot batch. It costs 24.2 ns against a table that does not fit the cache. So each entry
names the apparatus, the method, and the outcome. An entry that turned out to be invalid stays
here, and it says why.

The oldest entry is at the top, and a new entry goes at the bottom. This is the order of
a laboratory notebook. An entry records what happened at a time, and a later entry refers
to an earlier one. So the order must not change after the fact.

## Apparatus

Unless an entry says otherwise:

- One machine, 16 cores.
- The native benches are C, built at `-O2`, in `test/bench/`.
- The Elixir benches run with `mix run test/bench/<name>.exs`.
- The `stress bench` workflow gathers these on CI. It uploads `benchmarks-native`,
  `benchmarks-elixir`, and `stress-bench-cast`.

## Snapshots into the BEAM

`test/bench/data_plane.exs` and `test/bench/data_plane_ring.exs`. A digested snapshot
holds 8 entities.

| mechanism | snapshots/s |
| --- | --- |
| one message for each snapshot, the term copied into the mailbox | 1.38 M |
| shared ring, `:atomics` seqlock, 1 core | 2.85 M |
| shared ring, 2 cores | 5.68 M |
| shared ring, 4 cores | 11.2 M |
| shared ring, 8 cores | 21.6 M |
| shared ring, 16 cores | 27.7 M |
| one read from the ring | about 3 us |

A message for each snapshot copies a term into a mailbox, and it stops near 1.4 M each
second. The ring holds one slot that the writer overwrites and the BEAM samples. Each
zone has its own ring, so the rate rises with the cores.

## The same ring write, across the tiers

One write of a tick and 8 entities as fixed point, through the seqlock, on one core.

| tier | snapshots/s on one core | ns for each snapshot |
| --- | --- | --- |
| native C, the tier a plane runs in | 142.4 M | 7.0 |
| the BEAM ring, `Weft.DataPlane.Ring`, `:atomics` | 2.85 M | about 350 |
| the BEAM, one message for each snapshot | 1.38 M | about 725 |

A rate of 15 M snapshots each second is a budget of 66 ns for each snapshot. Only the
native tier fits it.

## The C++ ring, one writer for each core

`native/dataplane`, a seqlock ring, the same method as `test/bench/ring_native.c`. A
Ryzen 7 3800X, one writer for each core.

| threads, which are cores | aggregate snapshots/s | for each core |
| --- | --- | --- |
| 1 | 189.4 M | 189.4 M |
| 2 | 379.1 M | 189.5 M |
| 4 | 754.1 M | 188.5 M |
| 8 | 1498.9 M | 187.4 M |
| 16, with SMT | 2507.8 M | 156.7 M |

The rate scales nearly with the cores to 8 physical cores, at 7.9 times the rate of one
core. Each core owns its ring, so the planes share nothing. One core alone passes the
target of 15 M by more than 12 times.

## The iceoryx2 publish and subscribe bench

The own bench of iceoryx2, on a Ryzen 7 3800X. It pins two threads to two cores and
measures the round trip of the zero copy path. The pinning uses
`iceoryx2_bb_posix::thread::ThreadBuilder::affinity()`. Each run does 10 million round
trips from A to B to A.

| path | 12 B | 768 B | 8192 B |
| --- | --- | --- | --- |
| IPC zero copy, across processes | 236 ns | 233 ns | 239 ns |
| inside one process | 254 ns | 239 ns | — |
| IPC, the thread safe variant | 282 ns | 269 ns | 268 ns |
| IPC, `--send-copy` | — | 226 ns | — |

The one way latency is about 118 ns. It stays flat from 12 B to 8 kB, which is what
proves the copy does not happen. Only an offset crosses.

Across processes costs the same as inside one process. One serial link runs 2.11 million
round trips each second, and a streaming link runs far above the 15 M the ring produces.

This measured iceoryx2. weft runs iceoryx v1, and `../essays/runtime-choice.md` says why
the choice went the other way.

## Decode and apply, cache hot

`test/bench/pps_native.c`. A packet is 24 bytes of movement, replayed from a batch that
is already in the cache.

| cores | packets/s | ns for each packet for each core | ingress at 24 B |
| --- | --- | --- | --- |
| 1 | 826 M | 1.21 | 19.8 GB/s |
| 4 | 2.56 B | 1.57 | 61 GB/s |
| 8 | 5.09 B | 1.57 | 122 GB/s |
| 16 | 9.32 B | 1.72 | 224 GB/s |

This is the compute ceiling with the packets and the entities in L1 and L2. Real traffic
adds the DMA of the network card and a cache miss for each scattered entity.

## Decode and apply, against DRAM

`test/bench/pps_dram.c`. Random writes into an entity table of 2 GB, which is 64 M
entities, against an L3 of 32 MiB.

| cores | packets/s | ns for each operation for each core |
| --- | --- | --- |
| 1 | 41.2 M | 24.2 |
| 4 | 102.7 M | 38.9 |
| 8 | 115.5 M | 69.3 |
| 16 | 117.5 M | 136.1 |

The aggregate stops near 117 M each second, and 8 cores to 16 cores hardly moves it. That
is the wall of the DRAM bandwidth, and it is the honest ceiling for a large world.

## The SUMO trace

`test/bench/sumo/`. weft is the engine for a run of SUMO, which is a traffic
microsimulation. The city is a grid of 25 by 25 with dense traffic. Each vehicle is an
entity, and each step is a state frame.

The trace holds 600 frames, 11947 distinct vehicles, 8637 peak concurrent, and 2950620
entity updates.

Decode and apply on that trace, with `test/bench/sumo/replay.c`:

| cores | applies/s | ns for each apply for each core |
| --- | --- | --- |
| 1 | 840 M | 1.19 |
| 8 | 6.19 B | 1.29 |
| 16 | 7.78 B | 2.06 |

One core gives 840 M applies each second on real movement. The synthetic bench above
gave 826 M for one core, so the real data confirms the synthetic ceiling.

## The wire, on the same trace

`test/bench/sumo/encode_compare.py`, summed over the whole trace, at zstd level 1. The
column `z-dict` uses the frame before as the dictionary.

| format | bytes for each entity | raw | zstd | z-dict | z-dict against nasty |
| --- | --- | --- | --- | --- | --- |
| nasty, bitpacked | 12 | 35.4 MB | 25.8 MB | 12.6 MB | 1.0x |
| cheap, CBOR JSON-LD | 28 | 82.6 MB | 27.9 MB | 17.5 MB | 1.4x |

Raw, the cheap format costs 2.3 times the bytes. After zstd with the last frame as the
dictionary the gap falls to 1.4 times. CBOR JSON-LD repeats its field names in every
frame, and the dictionary removes that repetition.

## Replication bandwidth

`test/bench/zstd_frames.c`. A state frame holds 256 entities of 20 B, which is 5 kB.
Consecutive frames differ only where an entity moved. 30 percent of the entities move in
each frame.

| scheme | bytes for each frame | ratio | compress | decompress |
| --- | --- | --- | --- | --- |
| no dictionary, level 1 | 5130 | 1.0x | 0.3 M frames/s | — |
| the last frame as the dictionary, level 1 | 787 | 6.5x | 0.1 M frames/s | 0.1 M frames/s |

Level 1 and level 3 give about the same ratio here, so level 1 is the one to use. This
helps a replication frame, which is large and coherent. It does nothing for the 24 byte
input packets, which are small and independent.

## Invalid: the receive ceiling on loopback

`test/bench/udp_recv.c`. An attempt to measure how fast the kernel receives packets.

**Invalid. Do not cite it.** The result was about 0.16 M packets each second, and
`recvmmsg` gave no gain over `recv`. That absence of a gain is the tell. A batching call
that does not beat a call for each packet is not measuring the receive path. It is
measuring the send side of loopback, which throttles on ENOBUFS and one softirq core.

Loopback is not a proxy for a network card. The tool stays for a host that has one.

## Not measured

- **A real network card.** `test/bench/fly/netbench.c` is ready to run between two
  machines on Fly, and it is held until a run that is bounded in cost. Loopback cannot
  give this number, as the entry above shows.
- **Scattered entities under real traffic.** The two apply numbers bracket it. The truth
  is between the cache hot 1.21 ns and the DRAM bound 24.2 ns, and it depends on the
  layout of the entities and on the locality of the area of interest.
