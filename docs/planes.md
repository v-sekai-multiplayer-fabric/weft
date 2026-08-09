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

## VR headset

A SteamVR headset observing or playing a live world, with WebTransport in the HMD.
Two separations apply.

### Client, not a plane

A SteamVR HMD is a remote client across the network over WebTransport, not on
iceoryx2, so it is a client like Godot, not a plane. The word plane stays for
server-side iceoryx2 processes. The server-side parts that feed and consume the
headset's data are planes.

### Authority and interest, both

A HMD player is both an authority and an interest subscriber. This is the
authority/interest split, formalized in
[`lean-interest-mgmt`](https://github.com/v-sekai-multiplayer-fabric/lean-interest-mgmt).

- **Authority (upstream).** The player is the authority for their own avatar's tracked
  pose: head and hands. The tracker is the only source of that pose, so it originates
  at the HMD and flows upstream as authoritative data. World and physics authority
  stays server-side, one zone per entity (proven single-owner); the avatar pose is the
  part the client owns.
- **Interest (downstream).** The player has interest in the surrounding world and
  receives read-only area-of-interest replicas, served as `CH_INTEREST` snapshots. A
  peer can hold interest in an entity without authority over it; interest replicas do
  not consume authority slots and are bounded separately, with causal vector-clock
  staleness and k-tick lookahead.

A pure observer is the degenerate case: interest only, zero authority. The "observe
from SteamVR" ask is this case; a full VR player adds the avatar-pose authority
upstream.

### Transport

Both directions ride WebTransport. Live pose upstream and live world interest
downstream use unreliable drop-stale datagrams for the latest state (see `protocol.md`
and `latency.md`). Control and pulling baked OpenUSD stages from the asset CDN use
reliable streams.

### The feed is interest, and when it needs its own plane

The interest feed is always an interest producer, never authority. A single headset
can ride the game data plane's interest output directly. A dedicated spectator plane
(still interest, never authority) is warranted when the interest view must scale or
widen past per-peer area-of-interest culling:

- **Scale.** Many observers on a popular zone. The game data plane's reactor cores are
  pinned at 100% for the authority simulation, so interest fanout and encoding must
  not steal cycles from it. A spectator plane produces interest on its own cores.
- **A wider or enriched interest view.** A whole-zone or director camera, or overlays
  beyond per-peer area-of-interest culling (everyone's names and stats, event markers).
- **Delay and replay.** Broadcast spectating runs on a delay with scrubbing and replay
  to stop stream-sniping, a stateful interest buffer distinct from the live feed.

### Engine

The VR client is **Godot**, not a custom HMD client, consistent with "Godot stays on
the client." It uses Godot's OpenXR and SteamVR support and consumes WebTransport. The
engine build is
[`fabric-godot-core`](https://github.com/v-sekai-multiplayer-fabric/fabric-godot-core)
(the Godot fork), tagged by
[`fabric-godot-assembly`](https://github.com/v-sekai-multiplayer-fabric/fabric-godot-assembly)
(the multiplayer-fabric merge/assembly).
