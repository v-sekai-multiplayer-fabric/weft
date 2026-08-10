# Runtime choice

We set out to answer what looked like a straightforward question: for the data-plane
producer, is
[`fabric-stage-runtime`](https://github.com/v-sekai-multiplayer-fabric/fabric-stage-runtime)
faster, or is the Godot engine?

The benchmark answered a different question than the one we asked, which is the useful
kind of answer.

## What each actually is

- **fabric-stage-runtime = OpenUSD** (Pixar Universal Scene Description, 26.05), a
  C++ scene-_description/composition_ library shipped as a prebuilt per-triplet
  archive consumed through Elixir NIFs. It represents and composes world/scene
  state. It is not a physics tick engine.
- **Godot** is a full game engine: scene tree, physics, renderer, GDScript VM.
  Server-side it is heavyweight, and GDScript is interpreted and single-threaded.

## The budget

15M snapshots/sec is a **66 ns/snapshot** budget. Measured, same ring write op
(tick + 8 entities as fixed-point, seqlock) across tiers:

| Tier                                          | snapshots/sec (1 core) | ns/snapshot |
| --------------------------------------------- | ---------------------- | ----------- |
| **Native C** (the native plane tier)          | **142.4 M**            | 7.0         |
| BEAM ring (`Weft.DataPlane.Ring`, `:atomics`) | 2.85 M                 | ~350        |
| BEAM per-message (naive)                      | 1.38 M                 | ~725        |

Native clears 15M by ~9× on a single core. `test/bench/ring_native.c` reproduces it.

## The answer is neither

**Neither candidate is the producer.** Not "one is 20% better" — both are out by one to
five orders of magnitude. OpenUSD stage composition and Godot scene-tree operations cost
microseconds to milliseconds each, against a 66 ns budget.

When a comparison comes back that lopsided, the comparison was wrong. We had been
choosing a runtime for the hot loop from a shortlist that contained nothing capable of
running the hot loop. The producer has to be native code — Jolt physics and kernel-bypass
ingest writing the ring directly — and the BEAM samples it at about 3 µs per read. See
`Weft.DataPlane`.

They are not competing for the hot path — they are **stage-tier** choices (world
representation and authoring, at Hz). For the _server-side_ stage runtime,
**fabric-stage-runtime (OpenUSD) fits better than embedding Godot**: it is a headless
library (no renderer/input/VM baggage), composition- and interchange-oriented. It
runs as a plane over iceoryx v1 (a separate native process), not as an in-BEAM NIF,
because `Weft` forbids heavy C++ in the BEAM. Godot stays on the client.

So the layering the benchmark points to:

1. **Hot path (>15M/s):** the native game data plane and the ingest edge → `Weft.DataPlane.Ring`.
2. **Stage/world representation:** OpenUSD (fabric-stage-runtime), server-side, as a plane over iceoryx v1.
3. **Control plane:** weft (placement, single-writer, lifecycle, durable state).
4. **Client:** Godot (`fabric-godot-core`, tagged by `fabric-godot-assembly`), including the VR client via OpenXR/SteamVR over WebTransport.

A true OpenUSD-vs-Godot head-to-head would measure the _stage_ tier (stage mutation
and flatten rates), not the hot path; it needs the prebuilt OpenUSD archive and a
headless Godot build. Worth doing only to size the authoring tier, since neither is
on the 15M path.

## The asset pipeline is a CDN

The hot-path conclusion above stands: the >15M producer is the native game data plane,
not OpenUSD or Godot. The stage tier is a different job. The asset baker and the OpenUSD
stage tier together form weft's **asset CDN**:

- **Asset baker plane** (OpenUSD + Adobe glTF, request-response) bakes a source glb
  Character into an OpenUSD stage. This is a job: the control plane sends a bake
  request, the baker responds with a baked stage.
- **OpenUSD stage tier** (server-side) holds the baked stages and distributes them to
  clients like a CDN with casync, through the `desync` tool, the same format as
  `fabric-casync-central`. Content-defined chunking deduplicates at the chunk level, so
  a new stage version stores only its changed chunks, and a client pulls only the
  chunks it needs. The chunk store is not a plain directory or S3: the chunks are cut
  up into weft's store, SQLite to FoundationDB (the store plane), and served over an
  on-demand H3/WebTransport chunk endpoint, spawned when a client needs a stage and
  torn down when the transfer is done (scale-to-zero). It is not an HTTP/1.1 store,
  because the transport is H3/WebTransport. So the asset CDN and the actor store share
  one durable substrate.

Both run as planes over iceoryx v1, not in-BEAM NIFs, since `Weft` forbids heavy
C++ in the BEAM. Baking is off the game hot path; the >15M path stays native.

## Seastar or iceoryx2 with a thin harness (Windows support)

**Outcome: weft uses iceoryx v1.** The comparison below is kept because it is the
reasoning, not because iceoryx2 is still a candidate. The version is not a detail: v1
needs the RouDi daemon beside every plane, and iceoryx2 does not, so the choice changes
what a machine runs.

Seastar is Linux-only. It builds on epoll, io_uring, Linux AIO, and DPDK. A native
Windows plane cannot use it. iceoryx2 runs on Windows. So we asked one question. Can
iceoryx2 plus a small thread-per-core loop replace Seastar for a plane?

### The experiment

We ran iceoryx2's own publish-subscribe benchmark. It pins two threads to two cores and
measures the round-trip latency of the zero-copy path. The machine is a Ryzen 7 3800X.
The pinning uses `iceoryx2_bb_posix::thread::ThreadBuilder::affinity()`, an iceoryx2
building block. Each run does 10 million A to B to A round trips.

| Path                          | 12 B   | 768 B  | 8192 B |
| ----------------------------- | ------ | ------ | ------ |
| IPC zero-copy (cross-process) | 236 ns | 233 ns | 239 ns |
| Process-local (same process)  | 254 ns | 239 ns | —      |
| IPC, thread-safe variant      | 282 ns | 269 ns | 268 ns |
| IPC, `--send-copy` (768 B)    | —      | 226 ns | —      |

### What the numbers show

- The one-way latency is about 118 ns. That is very good on a 2019 desktop.
- The latency stays flat from 12 B to 8 KB. This proves true zero-copy. Only an offset
  crosses, not the payload.
- Cross-process IPC is as fast as same-process here. So a split into separate processes
  costs nothing versus Seastar's single-process model. This removes the main structural
  advantage of Seastar.
- One serial ping-pong link runs 2.11 million round trips per second. A streaming link
  runs far higher, well above the 15M snapshots per second the ring produces. So the
  transport does not bound the plane.

### The code cost of the harness

The hot path is about 30 to 40 lines. It creates a node, a service, a publisher, and a
subscriber. Then it loans, sends, and receives. The thread pinning is one call. It uses
`iceoryx2_bb_posix` on POSIX. On Windows it uses `std::thread` plus the `core_affinity`
crate.

### Does it fit every plane?

- **Game data plane** (publish-subscribe, CPU-bound, hot path). This is the ideal fit. A
  busy-poll thread-per-core loop over iceoryx2 matches the design.
- **Store plane, asset baker plane, and OpenUSD stage tier** (request-response,
  I/O-bound). iceoryx2 carries the transport. The execution is blocking worker threads,
  not a busy poll. This is simpler than Seastar's async I/O. It keeps I/O off the hot
  path, which the design already requires.
- **The network-ingest plane** (the one plane with networking on). It needs a QUIC or
  socket stack in any case. Seastar would give async sockets. Without Seastar, that one
  plane uses a Rust QUIC stack. Neither Seastar nor iceoryx2 gives WebTransport.

So iceoryx2 works as the transport for all planes. The busy-poll model fits the
CPU-bound hot planes. The I/O planes use blocking workers over the same transport.

### Is it less bloated than Seastar?

Yes, for weft. Seastar is a full runtime. It has a reactor, futures, a per-core
allocator, and a DPDK network stack. A weft plane uses the thread-per-core part and
little else. iceoryx2 plus `core_affinity` pulls in the transport and the pinning only.
So it is less code for what a plane needs. The Seastar parts we drop are the parts we do
not use.

### Tradeoff and recommendation

Seastar gave one runtime model for all planes (`Weft` rule 5). The iceoryx2 approach
is uniform at the transport. The execution differs by plane. It is busy-poll for CPU
planes and blocking workers for I/O planes.

Recommendation: if native Windows is a product requirement, adopt iceoryx2 plus a thin
thread-per-core harness, in Rust, and drop Seastar. The native planes are not built yet,
so the switch is cheap now and expensive later. This change rewrites `Weft` rule 5.
Treat that rewrite as an open question until we decide.

## Decision: iceoryx v1 in C++, because Rust is blocklisted

The target environment blocklists Rust. So the plane cannot use Rust or iceoryx2, since
iceoryx2 is Rust-first. The decision is iceoryx v1 (the C++ implementation) with a thin
C++ thread-per-core harness. This is the current design in `Weft` rule 5.

The cost of this decision, versus iceoryx2:

- iceoryx v1 needs the RouDi central daemon. iceoryx2 is brokerless.
- iceoryx v1 Windows support is experimental. iceoryx2 Windows support is better. So
  Linux is the primary target.
- The steady-state data path of both is zero-copy shared memory. So the latency is about
  the same, hundreds of ns to about 1 µs.

The C++ data plane meets the throughput goal with a wide margin. The ring is
`native/dataplane` (a seqlock ring, the same method as `test/bench/ring_native.c`). Measured
on a Ryzen 7 3800X, single writer per core:

| Threads (cores) | Aggregate snapshots/sec | Per core |
| --------------- | ----------------------- | -------- |
| 1               | 189.4 M                 | 189.4 M  |
| 2               | 379.1 M                 | 189.5 M  |
| 4               | 754.1 M                 | 188.5 M  |
| 8               | 1498.9 M                | 187.4 M  |
| 16 (with SMT)   | 2507.8 M                | 156.7 M  |

The pattern scales near-linearly to 8 physical cores, 7.9 times the single-core rate.
Each core owns its ring, so the planes share nothing. The 15M snapshots/sec goal is met
per core by more than 12 times.
