# Planes

The single governing rule of weft's architecture:

> **The BEAM runs only the control plane. Every other plane is a native process
> outside the VM, reached across shared memory.**

The BEAM is a conductor: placement, lifecycle, supervision, routing, distribution,
backpressure, caching. It is superb at coordination and terrible at sustained heavy
compute — so no heavy compute lives in it. Anything that parses, simulates,
crunches, or links a big third-party C++ library runs as its own **sidecar plane**.

## Why not "just a dirty NIF"

A NIF (even a dirty one) is still in the BEAM OS process, so:

- **A crash takes the whole node down.** NIF faults are unrecoverable; supervision
  cannot save you. A bad asset or a library bug must never kill the VM.
- **Its allocator, threads, and memory tangle with the VM's.**

That is the opposite of "let it crash / isolate faults." So heavy planes are
*out-of-process*, crash-isolated, and supervised by liveness, not linked in.

## The sidecar plane contract

Every non-control plane follows the same contract:

1. **Runs as its own native OS process(es)**, started by the container image
   (Docker / Fly), not spawned by the BEAM as a Port.
2. **Crash-isolated.** A plane fault never touches the BEAM node; the plane is
   restarted independently. The BEAM detects liveness, it does not share fate.
3. **Talks to the BEAM only through shared memory** — never a Port, never a socket
   round-trip on the hot path. Two boundary shapes cover everything:
   - **Ring** for streaming state (producer overwrites, BEAM samples at tick rate).
     See `Weft.DataPlane.Ring`.
   - **Queue** for request/response work (BEAM enqueues a job, the plane writes a
     result, BEAM polls/gets notified).
4. **The BEAM side is only a µs-scale poke.** The NIF that touches the shared
   segment does an `:atomics`/memcpy read or a queue enqueue and returns
   immediately. No long work, no busy-poll, no blocking a scheduler — the hard
   rules in `data-plane.md` hold for every plane.
5. **The BEAM orchestrates**: where a plane runs, its lifecycle, backpressure,
   caching of results, and routing. It owns the *what* and *where*; the plane owns
   the *how fast*.

## Planes today (and the shape they use)

| Plane | Native stack | Boundary | Isolation |
| --- | --- | --- | --- |
| **Control** | BEAM (weft) | — | supervised (OTP) |
| **Game data** | Seastar/DPDK + Jolt | ring | out-of-BEAM |
| **Asset baker** | OpenUSD + Adobe glTF plugin (fabric-stage-runtime) | queue | out-of-BEAM, crash-isolated |

New planes (physics, ML inference, video/audio transcode, …) join by implementing
the same contract — a native process + a ring or a queue. Nothing new is invented
per plane; the boundary is always shared memory and the BEAM is always just the
conductor.

## Consequences

- The BEAM stays lean and always responsive: it never does ms-scale work, so
  scheduler latency is bounded regardless of what the planes are doing.
- Any plane can be written in the best language for it (C++, Rust) and pinned to
  its own cores.
- A plane can be swapped, scaled, or crash without touching the control plane.
- Deployment is uniform: the container image bundles the plane binaries at fixed
  paths (so shared-lib resolution is deterministic), and Fly runs that image.
