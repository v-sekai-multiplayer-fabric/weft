# zone-server-h2o

This is a native `libh2o` + FoundationDB (FDB) zone server for the V-Sekai
multiplayer fabric. It replaces the Godot
`FabricZone`/`FabricZoneJournal`/`FabricMMOGZone` engine
(`V-Sekai-fire/multiplayer-fabric-build`, `godot/modules/multiplayer_fabric/`)
on the **server** side. The client stays Godot engine, unchanged. Only the
authoritative zone server moves.

## Status

This repo wires QUIC transport and H3/WebTransport session negotiation
(`src/transport/webtransport_server.c`, `src/transport/wt_session.c`). These
drive a real FDB-backed `ZoneTick` (`src/zf_zonetick.c`). Each process runs
exactly one zone. The zone ID comes from `main.c`'s required `-z<zone_id>`
flag, not a hardcoded value.

A zone fabric means multiple processes. Each process runs one zone (1
process : 1 zone). This matches `zone-server/AGENTS.md`'s deployment
shape: one UDP port per zone instance, up to 100 concurrent zones.
Avatar IK (`sinew-mocap/solve`'s `Align.lean`) is wired and tested; see
`rfd/0087` for that decision, and why MuJoCo was dropped for Godot's own
Jolt physics.

Three items are not done yet:
- The TLS cert and key are still `NULL`/`NULL`, so the server is
  unauthenticated.
- No real `cmake --build` against the actual linked libraries ran in this
  repo's own development yet. CI now does this build
  (`.github/workflows/real-build.yml`). Until this section says otherwise,
  treat it as freshly wired, not yet fully green.
- Cutover from the Godot deployment did not happen yet.

The event-loop, worker-pool, and FDB scaffold came from
[`weftspun/h2o-bench-tpcc`](https://github.com/weftspun/h2o-bench-tpcc). That
repo's TPC-C benchmark work is unrelated and stays there. Only the reusable
infrastructure, and the unimplemented `zonefabric` scenario design, carried
forward here.

### Concurrency and scaling

Each zone-server-h2o process runs its own FDB transaction per tick
(`zf_zonetick_run`, one call per process, one zone). RFD 0002's own
core-scaling argument depends on exactly this: "each core processes
independent zones... near-linear core scaling, unlike TPC-C where
district-level conflicts cause retries." `test/unit/test_zf_kv_multi_zone.c`
proves the FDB keyspace isolation many such processes rely on. The test uses
6 test zone IDs. No entity key from one zone ever falls inside another
zone's key range. So no process can ever read or write another zone's data
by accident.

**Not yet measured**: actual linear scaling across many concurrent
zone-server-h2o processes. This measurement needs a running FDB cluster,
several deployed processes, and a load generator (RFD 0013's `wrk`
harness). This repo's test suite cannot exercise that without live
infrastructure. This is a real, open verification gap, not a claim made and
left unchecked.

Coordinating many zone-server-h2o processes is a distinct, larger question.
Two examples of this: deciding which process owns which zone, and moving
an entity from one zone's process to another. `rfd/0086` tracks this
question and deliberately defers it. The team needs to revisit it now: the
deployment model is confirmed as multiple processes, not a single process
looping over several zones.

## Design provenance

This repo's design decisions live as RFDs in
[`multiplayer-fabric-manuals`](https://github.com/v-sekai-multiplayer-fabric/multiplayer-fabric-manuals/tree/main/rfd),
not inline here:

- `rfd/0083`: replaces the Godot `FabricZone` engine with this repo, and
  the overall entity/migration/ghost/journal shape it carries forward.
- `rfd/0086`: defers porting `NoGod.lean`'s gossip zone authority.
- `rfd/0087`: avatar IK uses `sinew-mocap/solve`'s `Align.lean`, not
  `kevinzakka/mink`; also the MuJoCo-to-Jolt physics drop.
- `rfd/0088`: transport is `picoquic` + `picotls`, not `h2o`'s own
  (absent) QUIC stack.
- `rfd/0072`, `rfd/0073`: the actor-lite architecture and async FDB
  callback chain the event loop, worker pool, and SPSC ring port as-is
  from `h2o-bench-tpcc`'s `src/`.

Entity and ReBAC types generate from `lean-entity-packet` and
`lean-rebac-core`, not hand-duplicated per language
(`src/gen/xr_grid_entity_packet.{c,h}`, `src/gen/rebac.{c,h}`), each
checked against that source Lean repo's own proved theorems or golden
vectors. Memory safety is built with
[Fil-C](https://github.com/pizlonator/fil-c) in CI
(`.github/workflows/build-filc.yml`); the FFI boundary against
`h2o`/`libfdb_c` (both still stock-compiled) is not resolved yet.

## Build

```sh
cmake -B build && cmake --build build
```

This build requires `libh2o` (evloop build) on the include/library path.
`libh2o`'s own build needs `libbrotli-dev` present at its build time. See
`h2o`'s `CMakeLists.txt`: it gates `libh2o-evloop`'s install rule on
finding Brotli.

This build also requires OpenSSL and the FoundationDB C client
(`libfdb_c`), both on the include/library path (see `CMakeLists.txt`).
It requires `mbedtls` too, built from source, not the system package.
Apt's `libmbedtls-dev` does not include `mbedtls_config.h`.

It also requires the vendored `thirdparty/` git subtrees (`picoquic`,
`picotls`, `QCBOR`), checked in directly (no separate init/fetch step),
built via `cmake/picoquic.cmake` / `cmake/qcbor.cmake`.
`.github/workflows/real-build.yml` runs this full build in CI. Check that
workflow's latest run for current status before assuming this build is
green.

## Verification

- `test/cbmc/spsc_harness.c`: a CBMC proof of the SPSC ring buffer, ported
  from `h2o-bench-tpcc` (RFD 0008).
- `test/verification/`: a Lean 4 + Plausible specification harness
  (`ZoneVerification.Spsc`), from the same source. Zonefabric-specific
  invariants (entity migration, ghost consistency, journal replay) will land
  here as those features are built.
