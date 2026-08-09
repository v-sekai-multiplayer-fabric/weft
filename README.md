# rivet_ex

Rivet's actor-orchestration control plane, ported to Elixir/OTP.

## Why

Rivet (Rust) drives serverless runner scaling from an accumulated counter
(`ServerlessDesiredSlotsKey`, `+1` on allocate / `-1` on destroy). Under churn the
two sites desync, the counter drifts negative, and the pool clamps desired runners
to zero **permanently** — a reachable, absorbing jam, machine-checked in
`rivet/engine/packages/pegboard/proofs/serverless_pool_jam.lean`
(`counter_can_jam`, `jam_is_sink`).

The fix in Rust was to stop counting and instead **observe live demand every tick**
(`reconciler_never_jams`). That is the OTP supervisor discipline by another name:
a supervisor never tallies how many children *should* exist, it holds a spec and
converges the live set. Porting the control plane to OTP makes the jam
*unrepresentable* rather than merely fixed, and hands us supervision, single-writer
process semantics, and distribution for free.

## Concept mapping (rivet → OTP)

| Rivet (Rust) | rivet_ex (OTP) |
| --- | --- |
| Actor: single-writer, KV + SQLite, addressed by `{name, key}` | `RivetEx.Actor` GenServer, one process per id via `RivetEx.Registry` |
| Pegboard exclusivity (lost-timeout + ping fencing) | Registry uniqueness + one-message-at-a-time mailbox; no distributed lease |
| `get_or_create` | `RivetEx.Actors.get_or_create/2` (race-safe) |
| `pegboard_runner_pool2` loop + `read_desired` | `RivetEx.Pool.Reconciler` + `RivetEx.Pool.desired_runners/2` |
| `ServerlessDesiredSlotsKey` counter | **deleted** — demand is observed, never accumulated |
| Serverless runner / outbound `/start` | `RivetEx.Pool.Runner` |
| `Bump` signal | `Reconciler.bump/1` |
| Reschedule on runner loss | monitor `:DOWN` → re-observe → re-converge (self-heal) |

## Implemented

- Single-writer actors under a supervision tree, addressed through a registry,
  with race-safe `get_or_create` that tolerates the registry-cleanup window.
- **Durable per-actor state**: one SQLite database per actor, write-through,
  restored on restart. The single writer means no locking or lease.
- **Scale-to-zero lifecycle**: an idle actor stops and releases its process, then
  the next request wakes a fresh process that restores state from disk.
- **Cluster-wide single writer + failover** (Horde): one actor instance per id
  across the cluster, addressable from any node, handed off to a survivor when its
  host node dies. Proven with a real multi-node `:peer` cluster (`cluster_test.exs`),
  including durable state surviving the handoff.
- **FoundationDB store backend**: node-independent durable state (no filesystem
  affinity), so an actor that migrates machines still reads its data. Same choice
  rivet makes. Selectable via `:actor_store`; tested against a live FDB (`:fdb`).
- **Control/data-plane boundary** (`docs/data-plane.md`): a `RivetEx.Zone` owns a
  per-zone data-plane worker behind a behaviour, receiving digested snapshots
  event-driven and steering it, never polling or touching a packet. A stub worker
  stands in for the C++ Seastar + iceoryx + Jolt stack; the contract is identical.
- The level-triggered pool reconciler: `desired = clamp(margin + ceil(demand /
  slots), min, max)`, converging the live runner set with no counter; self-heals
  when a runner crashes.
- Tests mirror the Rust proof and integration test: `desired_runners/2` unit tests,
  a `reconciler_never_jams` **property**, the scale-up-on-demand test (Elixir
  counterpart of `serverless_pool_reconcile.rs`), crash self-heal, drain, durable
  restart, and idle sleep/wake.

## Quality gates

```sh
mix test        # unit + property tests
mix dialyzer    # success typing, strict flags (error_handling, extra_return,
                # missing_return, unmatched_returns)
```

Both are green. Dialyzer is the BEAM analogue of the Lean spec: it proves the
call graph is type-consistent, and it has already caught unhandled return values
and an over-narrow `@spec` in this code.

## Roadmap

Ordered by how load-bearing each piece is in rivet.

1. ~~**Durable per-actor state**~~ — done: SQLite per actor.
2. ~~**Lifecycle**~~ — done: idle sleep, wake on request, scale-to-zero.
3. ~~**Distribution**~~ — done: cluster-wide single writer + handoff via Horde.
4. ~~**Distributed store backend**~~ — done: FoundationDB store, node-independent.
5. ~~**Data-plane boundary**~~ — done (prototype): `RivetEx.Zone` + worker behaviour
   + stub. See `docs/data-plane.md`.
6. **Real data-plane worker** — implement `RivetEx.DataPlane.Worker` as a Port/NIF
   to the C++ **Seastar (DPDK) + iceoryx + Jolt** stack: 15M+ pps ingest, zero-copy
   IPC, 60Hz physics, pushing digested snapshots to the zone. The BEAM contract is
   already fixed.
7. **Gateway / routing** — address an actor/zone and forward a request.
8. **Workflow engine** — durable multi-step operations (gasoline → Oban or a
   custom OTP saga) for anything needing replay/observability.

The real-time spatial data plane stays outside the BEAM (C/C++/Rust); rivet_ex is
the control plane that places zones and holds their durable state.

## Non-goals

rivet_ex is not the real-time data fabric. High-rate entity/transform replication,
voice, and edge/microcontroller reach belong on Zenoh (`zenoh-pico`). This project
owns durable stateful actors, lifecycle, scheduling, and supervision.
