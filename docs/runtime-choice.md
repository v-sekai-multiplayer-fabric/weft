# Runtime choice for the >15M snapshots/sec goal

Question: for the data-plane producer, is it faster to use
[`fabric-stage-runtime`](https://github.com/v-sekai-multiplayer-fabric/fabric-stage-runtime)
or the Godot engine? We let the benchmark lead.

## What each actually is

- **fabric-stage-runtime = OpenUSD** (Pixar Universal Scene Description, 26.05), a
  C++ scene-*description/composition* library shipped as a prebuilt per-triplet
  archive consumed through Elixir NIFs. It represents and composes world/scene
  state. It is not a physics tick engine.
- **Godot** is a full game engine: scene tree, physics, renderer, GDScript VM.
  Server-side it is heavyweight, and GDScript is interpreted and single-threaded.

## The budget

15M snapshots/sec is a **66 ns/snapshot** budget. Measured, same ring write op
(tick + 8 entities as fixed-point, seqlock) across tiers:

| Tier | snapshots/sec (1 core) | ns/snapshot |
| --- | --- | --- |
| **Native C** (the Jolt/Seastar tier) | **142.4 M** | 7.0 |
| BEAM ring (`Weft.DataPlane.Ring`, `:atomics`) | 2.85 M | ~350 |
| BEAM per-message (naive) | 1.38 M | ~725 |

Native clears 15M by ~9× on a single core. `bench/ring_native.c` reproduces it.

## Conclusion

**Neither fabric-stage-runtime nor Godot is the >15M producer.** OpenUSD stage
composition and Godot scene-tree/physics ops run at microseconds to milliseconds
each — one to five orders of magnitude over the 66 ns budget. The hot loop is
**native** (Jolt physics + Seastar/DPDK ingest) writing the ring; the BEAM samples
it (~3 µs/read, `docs/data-plane.md`).

They are not competing for the hot path — they are **stage-tier** choices (world
representation and authoring, at Hz). For the *server-side* stage runtime consumed
by weft's control plane, **fabric-stage-runtime (OpenUSD via Elixir) fits better
than embedding Godot**: it is a headless library (no renderer/input/VM baggage),
composition- and interchange-oriented, and already packaged for the BEAM. Godot
stays on the client.

So the layering the benchmark points to:

1. **Hot path (>15M/s):** native Jolt/Seastar → `Weft.DataPlane.Ring`.
2. **Stage/world representation:** OpenUSD (fabric-stage-runtime) via Elixir, server-side.
3. **Control plane:** weft (placement, single-writer, lifecycle, durable state).
4. **Client:** Godot.

A true OpenUSD-vs-Godot head-to-head would measure the *stage* tier (stage mutation
and flatten rates), not the hot path; it needs the prebuilt OpenUSD archive and a
headless Godot build. Worth doing only to size the authoring tier, since neither is
on the 15M path.
