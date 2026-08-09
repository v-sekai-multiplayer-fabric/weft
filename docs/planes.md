# Planes

The main rule of weft's architecture:

> **The BEAM runs only the control plane. Every other plane is a native process
> outside the BEAM. The BEAM reaches it through Eclipse iceoryx (zero-copy IPC).**

The BEAM does coordination: placement, lifecycle, supervision, routing,
backpressure, and caching. It is good at coordination and bad at heavy compute.
So no heavy compute runs in it. Any work that parses, simulates, crunches, or links
a large C++ library runs as its own native process. We call each one a **plane**.

## Why not a dirty NIF

A NIF, even a dirty one, still runs inside the BEAM OS process. That has two
problems:

- **A crash kills the whole BEAM.** NIF crashes cannot be caught. Supervision does
  not help. A bad input or a library bug must not take down the BEAM.
- **The NIF's memory and threads mix with the BEAM's.**

So heavy planes run as separate processes. A plane can crash on its own and be
restarted without touching the BEAM.

## The plane contract

Every plane that is not the control plane follows the same rules:

1. **It is a separate native OS process.** The container image (Docker / Fly)
   starts it. The BEAM does not start it as a Port.
2. **It is sandboxed and crash-isolated.** Each plane runs in a
   [bubblewrap](https://github.com/containers/bubblewrap) sandbox: restricted
   filesystem, namespaces, and seccomp. It can reach only what it is given — the
   iceoryx segment, its own binary, and its data — not the host, the BEAM, or other
   planes. Because planes talk over iceoryx, not the network, a plane that does not
   need the network runs with networking off (`--unshare-net`). This matters for
   planes that handle untrusted input: the baker parses arbitrary glb files, so with
   no network a broken or exploited baker cannot reach out. It is also
   crash-isolated: if it crashes, the BEAM keeps running and the plane is restarted
   on its own. The BEAM checks liveness only. On Fly this is a second layer inside
   the machine's container.
3. **It talks to the BEAM only through Eclipse iceoryx** (zero-copy IPC). Never a
   Port. Never a socket on the hot path. Two iceoryx patterns cover all cases:
   - **Publish-subscribe** for streaming state. The plane publishes samples; the
     BEAM subscribes and takes the latest.
   - **Request-response** for jobs. The BEAM sends a request; the plane responds.
4. **The BEAM side is a small iceoryx NIF.** It takes a sample or sends a request,
   copies the bytes into a BEAM binary, and returns at once. No long work, no
   busy-poll, no blocked scheduler. The hard rules in `data-plane.md` apply to
   every plane.
5. **If a plane needs an event loop, it uses Seastar** (thread-per-core,
   shared-nothing). Not every plane needs one. A plane that does async I/O,
   networking, or many concurrent tasks uses Seastar. A simple stateless converter
   can be a plain loop.
6. **The control plane orchestrates.** It decides where a plane runs, its lifecycle,
   backpressure, and result caching. The BEAM owns _what_ and _where_. The plane
   owns _how fast_.

## Planes today

| Plane       | Native stack                                       | iceoryx pattern   | Isolation                   |
| ----------- | -------------------------------------------------- | ----------------- | --------------------------- |
| Control     | BEAM (weft)                                        | —                 | supervised (OTP)            |
| Game data   | Seastar/DPDK + Jolt                                | publish-subscribe | out of BEAM                 |
| Asset baker | OpenUSD + Adobe glTF plugin (fabric-stage-runtime) | request-response  | out of BEAM, crash-isolated |

New planes (physics, ML inference, video/audio transcode) use the same contract: a
native process plus iceoryx publish-subscribe or request-response. There is nothing
new to design per plane.

Seastar is the event loop, not a plane by itself. In the game data plane, Seastar
runs the loop, drives Jolt (physics), and reaches the control plane and other
planes through iceoryx. A plane that needs an event loop uses Seastar the same way.

## Durable state: FoundationDB

iceoryx is a local bus. It connects planes on the same machine with zero copy. It
does not cross machines and it does not store anything.

Durable, cross-machine state lives in **FoundationDB**. The control plane reads and
writes it with the FoundationDB client (`erlfdb`) over the network. FoundationDB holds actor state,
zone state, and entity ownership. It survives crashes, and it lets an actor or zone
move to another machine and still find its data. FoundationDB is not a plane and is
not on iceoryx. It is the shared source of truth below the planes.

Two boundaries, two jobs:

- **iceoryx**: same machine, zero-copy, hot path, between planes.
- **FoundationDB**: across machines, durable state, over the network.

## Results

- The BEAM stays fast. It never does slow work, so scheduler latency stays low no
  matter what the planes do.
- Each plane can use the best language for the job (C++, Rust) and its own CPU
  cores.
- A plane can be replaced, scaled, or crash without touching the control plane.
- Deployment is the same for all planes: the container image holds the plane
  binaries at fixed paths, so shared-library paths are known. Fly runs that image.
