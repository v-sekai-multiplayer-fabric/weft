# Protocol

weft moves entity state in two fixed formats, one per layer. This is not a choice
made per message. Each layer always uses its one format.

- **Nasty, on the hot path.** A bitpacked C struct. Per entity update: a 4-byte
  slot id and two 4-byte floats for position, 12 bytes total. Decode is a cast: the
  bytes are already the struct, so applying an update is a slab write with no parse
  and no allocation. This is the format for the data plane, replication, and every
  path that runs at packet rate.
- **Cheap, at the interop edge.** CBOR-encoded JSON-LD. Self-describing, with named
  fields and a context, so other tools can read it without weft's struct layout.
  This is the format for debugging, inspection, and exchange with outside systems.
  It never runs on the hot path.

The rule: the hot path is always nasty, the interop edge is always cheap. Latency
decides the hot path, so it takes the format that decodes by cast. Interoperability
decides the edge, so it takes the self-describing format, and it is off the hot path
where its cost does not matter.

## Evidence: the SUMO trace

We measured both formats on a real workload, not synthetic data. SUMO (Eclipse
traffic microsimulation) ran a 25 by 25 grid city with dense traffic. Each vehicle
is a weft entity and each simulation step is a state frame. The trace is 600 frames,
11,947 distinct vehicles, 8,637 peak concurrent, 2,950,620 entity updates.
Reproduce with `test/bench/sumo/` (see `README` there).

### Wire size

Summed over the whole trace, level 1 zstd, "z-dict" uses the previous frame as the
dictionary (the last-frame delta):

| Format               | bytes/entity | raw     | zstd    | z-dict  | z-dict vs nasty |
| -------------------- | ------------ | ------- | ------- | ------- | --------------- |
| nasty (bitpacked)    | 12           | 35.4 MB | 25.8 MB | 12.6 MB | 1.0x            |
| cheap (CBOR JSON-LD) | 28           | 82.6 MB | 27.9 MB | 17.5 MB | 1.4x            |

Raw, cheap is 2.3 times the bytes. After zstd with the last-frame dictionary the gap
falls to 1.4 times, because CBOR JSON-LD repeats its field names every frame and
zstd removes that repetition. So on the wire the size cost of cheap is small. The
real cost of cheap is not size, it is decode.

### Decode and apply speed

The nasty decode plus apply, measured in C on the real trace (`test/bench/sumo/replay.c`),
with the entity slab resident in L2:

| cores | pps    | ns/apply/core |
| ----- | ------ | ------------- |
| 1     | 840 M  | 1.19          |
| 8     | 6.19 B | 1.29          |
| 16    | 7.78 B | 2.06          |

840 M applies per second on one core is 56 times the 15 M packets per second target.
This matches the synthetic `test/bench/pps_native.c` number (826 M per core), so real
traffic movement confirms the synthetic benchmark. Apply is never the bottleneck.

Cheap decode is a parse, not a cast, so it is far slower and allocates per field.
That is acceptable at the interop edge and is the reason cheap never touches the hot
path. See `../essays/latency.md` for why the hot path stays native and copy-free.
