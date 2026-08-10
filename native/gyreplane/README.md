# zone-server-h2o

This is a native FoundationDB (FDB) zone server for the V-Sekai
multiplayer fabric. The name keeps `h2o` and the code does not: see the
weft fork note below. It replaces the Godot
`FabricZone`/`FabricZoneJournal`/`FabricMMOGZone` engine
(`V-Sekai-fire/multiplayer-fabric-build`, `godot/modules/multiplayer_fabric/`)
on the **server** side. The client stays Godot engine, unchanged. Only the
authoritative zone server moves.

## Status

**weft fork.** Two things left this directory, and neither is coming
back.

**The transport.** A plane has no networking, so the QUIC and
H3/WebTransport termination moved to `../edge`, together with `picoquic`
and `picotls`. This process opens no socket.

It did not move as one piece. `Weft` names two edges, and the split is by
what the traffic is. The **ingest edge** carries player input datagrams,
which are unreliable and high rate, and it feeds the game data plane. The
**gateway edge** carries control streams, which are reliable and low
rate, and it feeds the control plane. A client holds one session to each,
so control work cannot delay the datagrams.

**h2o.** It outlived the transport by one change, kept for its event
loop. That loop drove nothing: `fdb_thread_init` stored an `h2o_loop_t`
in `fdb_thread_state_t`, and no code ever read it back, so a worker
thread ran `h2o_evloop_run` on a loop with no handle registered. This
build links no h2o at all. The worker threads wait rather than spin.

Nothing drives the `ZoneTick` until iceoryx2 carries the decoded input
from an edge. That is the next thing to build, and
`../harness/README.md` holds the bus it will use. See `WEFT.md`.

This repo drives a real FDB-backed `ZoneTick` (`src/zf_zonetick.c`).
Each process runs
exactly one zone. The zone ID comes from `main.c`'s required `-z<zone_id>`
flag, not a hardcoded value.

A zone fabric means multiple processes. Each process runs one zone (1
process : 1 zone). This matches `zone-server/AGENTS.md`'s deployment
shape of up to 100 concurrent zones. The one UDP port per zone
instance that shape also named is the edge's job now.
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

The worker pool went in the weft fork. It dispatched `h2o_req_t` over an
`h2o_multithread` queue, and a plane answers no request. `src/utility.c`,
`src/thread.c`, `src/event_loop.h`, and the HTTP half of `src/error.h`
went with it, for the same reason. `src/spsc_ring.c` went too, once its
last caller was gone. It queues `void *`, and a process-local pointer
cannot cross a shared-memory segment that maps at a different address in
each process. iceoryx solves that problem, and it carries its own
lock-free queue.

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
  (absent) QUIC stack. That transport lives in `../edge` now, split into
  an ingest edge and a gateway edge.
- `rfd/0072`, `rfd/0073`: the actor-lite architecture and async FDB
  callback chain port from `h2o-bench-tpcc`'s `src/`. The event loop, the
  worker pool, and the SPSC ring went in the weft fork.

Entity and ReBAC types generate from `lean-entity-packet` and
`lean-rebac-core`, not hand-duplicated per language
(`src/gen/xr_grid_entity_packet.{c,h}`, `src/gen/rebac.{c,h}`), each
checked against that source Lean repo's own proved theorems or golden
vectors. Memory safety is built with
[Fil-C](https://github.com/pizlonator/fil-c) in CI
(`.github/workflows/build-filc.yml`); the FFI boundary against
`libfdb_c`, which is still stock-compiled, is not resolved yet.

## Build

```sh
cmake -B build && cmake --build build
```

This build requires OpenSSL and the FoundationDB C client (`libfdb_c`),
both on the include/library path (see `CMakeLists.txt`). It no longer
requires `libh2o`, and with it went `libbrotli-dev`, which existed only
because h2o's own build gates `libh2o-evloop`'s install rule on finding
Brotli.
There is no `thirdparty/` any more, and no `cmake/` either. QCBOR lost
its last caller upstream, `libriscv` went with the guest sandbox, and
`picoquic` and `picotls` went to `../edge`. This build links system
libraries only.
`.github/workflows/real-build.yml` runs this full build in CI. Check that
workflow's latest run for current status before assuming this build is
green.

## Verification

`test/cbmc/` and `test/verification/` held a CBMC proof and a Lean 4 plus
Plausible specification of the SPSC ring, both ported from
`h2o-bench-tpcc` (RFD 0008). Both went with the ring they proved. A proof
of code that is not here proves nothing about what runs.

weft keeps its specifications in `docs/spec/`, and `docs/spec/README.md`
explains how a test mirrors a proof. A queue invariant belongs there if
weft ever writes its own queue. It does not, because iceoryx2 has one.
