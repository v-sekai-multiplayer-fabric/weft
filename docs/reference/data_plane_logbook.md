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

## The transport weft did not take

The round trip of the transport that was evaluated and rejected is not here. It measures a
product weft does not use, and the name of that product is retired. A retired term may
appear only in the page that records why it was retired, which the vocabulary test
enforces. So that measurement stays in `../essays/runtime-choice.md`, beside the decision
it informed.

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

## What the dlsym dispatch table costs

`native/harness/src/bench_send.cpp`, built three ways from one source. The loop is loan,
write 40 bytes, send, receive, drop, with a subscriber in the same process so the send
path is a real send path. 200000 messages for each pass, 7 passes, median of the passes,
and the run repeated 4 times.

One machine, 16 cores, Fedora, GCC 16.1.1 at `-O2`, iceoryx2 v0.9.3, Release.

| build | ns for each message | against shared |
| --- | --- | --- |
| dlsym table, from `iceoryx2.sigs` | 428 | +1.1% |
| direct, shared library | 423 | — |
| direct, static library | 418 | -1.2% |

The loop makes 5 iceoryx2 calls for each message, so the table costs about 1 ns for each
call. That is one indirect call, which is what it is.

Read the shared row and not the static row. A shared link already pays an indirect jump
through the PLT, so it is the alternative weft would otherwise have. The static row is the
strictest baseline and the least realistic, because it is the only build where the
compiler could inline across the call, and it does not link a Rust artifact the way weft
would have to.

The effect is close to the run-to-run spread. Four repeats of the shared build ranged 421
to 428 ns, and the gap to the table is about 5 ns. So this bounds the cost rather than
measuring it precisely: it is small, and it is not zero.

The payload never crosses the table. `loan` returns a pointer into shared memory, and the
write is a `memcpy` the caller does. So the cost is for each call and not for each byte,
and a larger message does not pay more.

### The batch size the 15 M target needs

Same apparatus. `iox2_publisher_loan_slice_uninit` loans a slice, so one message carries a
batch whatever its length, at one loan and one send.

| entities in a message | ns for each message | M snapshots/s |
| --- | --- | --- |
| 1 | 420.3 | 2.38 |
| 8 | 424.5 | 18.85 |
| 32 | 454.9 | 70.35 |
| 64 | 497.7 | 128.58 |
| 256 | 730.2 | 350.61 |
| 1024 | 1695.1 | 604.11 |

**A batch of 8 clears the 15 M target on one core.** A batch of 7 is the break-even point,
and nothing above it is close.

The first two rows are the reason. 1 to 8 entities costs 4 ns more for each message, so
almost the whole 420 ns is fixed cost that a batch pays once. Above 32 the payload write
starts to show, and from 8 to 1024 the marginal cost settles near 1.25 ns for each entity.

So the bus is not the constraint at this target. The batch size is, and the batch size a
frame already has is 256.

### The bus is not the per-snapshot path

428 ns for each message is 2.3 M messages each second on one core. The C++ ring above does
189.4 M snapshots each second on one core, which is 5.3 ns each. The bus is about 80 times
slower than the ring, and the two numbers are not comparable work.

So one iceoryx2 message for each snapshot is not a design. At the 15 M snapshots each
second target it would need about 6.4 cores for the bus alone, and the ring needs a
fraction of one. The table above shows what fixes it, and it is one number: 8.

A message carries a frame, and a frame carries many entities. The replication entry above
uses 256 entities for each frame. At that size, 15 M snapshots each second is 58.6 K
messages each second, which is about 25 µs of bus time in each second on one core. The
dlsym table takes about 1.1% of that.

This is the reason the ring exists beside the bus, and it is worth stating plainly: the
ring carries state at packet rate, and the bus carries frames and commands.

## Not measured

- **A real network card.** `test/bench/fly/netbench.c` is ready to run between two
  machines on Fly, and it is held until a run that is bounded in cost. Loopback cannot
  give this number, as the entry above shows.
- **The bus under load.** The table above is one publisher and one subscriber in one
  process. A real plane has one for each core, and the numbers will differ.
- **Scattered entities under real traffic.** The two apply numbers bracket it. The truth
  is between the cache hot 1.21 ns and the DRAM bound 24.2 ns, and it depends on the
  layout of the entities and on the locality of the area of interest.
