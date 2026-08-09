# Planes

The main rule of weft's architecture:

> **The BEAM runs only the control plane. Every other plane is a native process
> outside the BEAM. The BEAM reaches it through Eclipse iceoryx2 (zero-copy IPC).**

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
   iceoryx2 segment, its own binary, and its data — not the host, the BEAM, or other
   planes. Because planes talk over iceoryx2, not the network, **networking is off by
   default** (`--unshare-net`). The one exception is the plane that ingests from the
   network (the game data plane). This matters for a plane that handles untrusted
   input: the asset baker parses arbitrary glb files, so with no network a broken or
   exploited baker cannot reach out. It is also
   crash-isolated: if it crashes, the BEAM keeps running and the plane is restarted
   on its own. The BEAM checks liveness only. On Fly this is a second layer inside
   the machine's container.
3. **It talks to the BEAM only through Eclipse iceoryx2** (zero-copy IPC). Never a
   Port. Never a socket on the hot path. Two iceoryx2 patterns cover all cases:
   - **Publish-subscribe** for streaming state. The plane publishes samples; the
     BEAM subscribes and takes the latest.
   - **Request-response** for jobs. The BEAM sends a request; the plane responds.
4. **The BEAM side is a small iceoryx2 NIF.** It takes a sample or sends a request,
   copies the bytes into a BEAM binary, and returns at once. No long work, no
   busy-poll, no blocked scheduler. The hard rules in `data-plane.md` apply to
   every plane.
5. **A plane is a Seastar app.** Every plane runs on Seastar (thread-per-core,
   shared-nothing). There is one runtime model for all planes, not a choice per
   plane.
6. **The control plane orchestrates.** It decides where a plane runs, its lifecycle,
   backpressure, and result caching. The BEAM owns _what_ and _where_. The plane
   owns _how fast_.

## Planes today

| Plane       | Native stack                                       | iceoryx2 pattern  | Isolation                   |
| ----------- | -------------------------------------------------- | ----------------- | --------------------------- |
| Control     | BEAM (weft)                                        | —                 | supervised (OTP)            |
| Game data   | Seastar/DPDK + Jolt                                | publish-subscribe | out of BEAM                 |
| SUMO        | Eclipse SUMO traffic microsimulation               | publish-subscribe | out of BEAM                 |
| Asset baker | OpenUSD + Adobe glTF plugin (fabric-stage-runtime) | request-response  | out of BEAM, crash-isolated |

The SUMO plane is the current focus. It streams per-step entity movement into the
ring, the game data plane's publish-subscribe pattern.

The asset baker plane plus the OpenUSD stage tier form weft's **asset CDN**: the
baker bakes a source glb into a content-addressed OpenUSD stage (request-response),
and the stage tier caches and distributes baked stages to clients like a CDN. Baking
is off the game hot path. See `runtime-choice.md`.

New planes (physics, ML inference, video/audio transcode) use the same contract: a
native process plus iceoryx2 publish-subscribe or request-response. There is nothing
new to design per plane.

Seastar is the event loop every plane runs on, not a plane by itself. In the game
data plane, Seastar runs the loop, drives Jolt (physics), and reaches the control
plane and other planes through iceoryx2. Every plane uses Seastar the same way.

## Durable state: FoundationDB

iceoryx2 is a local bus. It connects planes on the same machine with zero copy. It
does not cross machines and it does not store anything.

Durable, cross-machine state lives in **FoundationDB**. The control plane reads and
writes it with the FoundationDB client (`erlfdb`) over the network. FoundationDB holds actor state,
zone state, and entity ownership. It survives crashes, and it lets an actor or zone
move to another machine and still find its data. FoundationDB is not a plane and is
not on iceoryx2. It is the shared source of truth below the planes.

Two boundaries, two jobs:

- **iceoryx2**: same machine, zero-copy, hot path, between planes.
- **FoundationDB**: across machines, durable state, over the network.

## Results

- The BEAM stays fast. It never does slow work, so scheduler latency stays low no
  matter what the planes do.
- Each plane can use the best language for the job (C++, Rust) and its own CPU
  cores.
- A plane can be replaced, scaled, or crash without touching the control plane.
- Deployment is the same for all planes: the container image holds the plane
  binaries at fixed paths, so shared-library paths are known. Fly runs that image.

## VR observer

We want to observe a live world from SteamVR, with WebTransport running in the HMD.
This splits in two, and the split is settled:

- **The HMD is a client.** A SteamVR HMD is a remote client across the network over
  WebTransport, not on iceoryx2, so it is a client like Godot, not a plane. The word
  plane stays for server-side iceoryx2 processes.
- **The part that provides the observer's data is a plane.** That is server-side. The
  game data plane already produces digested world state, so its output is the
  observer feed; the gateway forwards it to the HMD over WebTransport.

A single read-only VR headset needs no new plane: it consumes the game data plane's
full digest over WebTransport. A dedicated spectator plane is added only when a
concrete trigger appears:

- **Scale.** Many observers on a popular zone. The game data plane's reactor cores
  are pinned at 100% for the simulation, so spectator fanout and encoding must not
  steal cycles from it. A spectator plane reads the output and scales on its own
  cores.
- **A wider or enriched view.** The authoritative feed is area-of-interest culled per
  participant and omits what players must not see. A whole-zone or director camera,
  or overlays players never get (everyone's names and stats, event markers), is extra
  work on an un-culled view.
- **Delay and replay.** Broadcast spectating runs on a delay with scrubbing and
  replay to stop stream-sniping, a stateful buffer distinct from the live feed.

Until one of these holds, there is no spectator plane.

### Open questions

1. **Transport.** Real-time observation uses unreliable drop-stale datagrams for the
   latest entity and pose state (see `protocol.md` and `latency.md`); control and
   pulling baked OpenUSD stages from the asset CDN use reliable streams. Confirm.
2. **Role.** Observe only (read-only) first. Later, does it send input and become a
   participant, or stay a pure observer?
3. **Engine.** Godot already has OpenXR and SteamVR support, so the VR client may be
   Godot plus OpenXR consuming WebTransport, consistent with "Godot stays on the
   client." Or a custom WebTransport HMD client. Decide one.
