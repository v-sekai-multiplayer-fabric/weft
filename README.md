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
3. **Distribution** — multi-node single-writer via `Horde.Registry` +
   `Horde.DynamicSupervisor` (or `:syn`), replacing pegboard's cross-node fencing.
   This is the part that proves OTP can do pegboard's hardest job.
4. **Gateway / routing** — address an actor and forward a request (Phoenix or plug).
5. **Workflow engine** — durable multi-step operations (gasoline → Oban or a
   custom OTP saga) for anything needing replay/observability.
6. **Data plane** — keep the real-time spatial plane (entity replication, interest
   management, edge datagrams) on **Eclipse Zenoh**, not here. rivet_ex is the
   control plane; Zenoh is the data plane.

## Non-goals

rivet_ex is not the real-time data fabric. High-rate entity/transform replication,
voice, and edge/microcontroller reach belong on Zenoh (`zenoh-pico`). This project
owns durable stateful actors, lifecycle, scheduling, and supervision.
