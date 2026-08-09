# Planes

The main rule of weft's architecture:

> **The BEAM runs only the control plane. Every other plane is a native process
> outside the BEAM. The BEAM reaches it through Eclipse iceoryx2 (zero-copy IPC).**

The BEAM does coordination: placement, lifecycle, supervision, routing,
backpressure, and caching. It is good at coordination and bad at heavy compute.
So no heavy compute runs in it. Any work that parses, simulates, crunches, or links
a large C++ library runs as its own native process. We call each one a **plane**.

## Terms

One name per concept. The character terms are the Khronos 3D Formats Characters and
Avatars TSG definitions (see `CITATION.cff`); the runtime terms are weft's.

Runtime:

- **actor**: a weft runtime process with a single writer. The control-plane
  primitive.
- **zone**: an actor that owns one spatial partition and simulates the entities in
  it. A zone is a kind of actor. The whole set of zones is the shard.
- **entity**: one simulated thing inside a zone, with position and velocity. The unit
  the game data plane moves, thousands per zone. An entity is not an actor. Authority
  is per entity: each entity is authoritative on exactly one zone, and a zone owns
  many entities, so it is not one zone per entity.

Character domain (Khronos CATSG):

- **Character**: a 3D asset representing a potentially animatable figure (human,
  animal, creature), including metadata about usage of the model. The asset CDN bakes
  and stores Characters.
- **Avatar**: a Character controlled by a controller to embody them in the 3D world. A
  player's embodiment is an avatar; its per-tick runtime state (pose, position)
  travels as an entity.
- **controller**: the human or AI that controls an avatar. The TSG names this concept
  "entity"; weft says controller, because entity already names the runtime sim unit
  above. This keeps one name per concept.
- **player**: a controller that is a human.
- **Persona**: a controller's personality expressed through their avatar.
- **Identity**: the controller embodying an avatar, plus the data that identifies them
  (authentication, verification).

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
| Store       | native SQLite (WAL) + FoundationDB replica         | request-response  | out of BEAM, crash-isolated |

The SUMO plane is the current focus. It streams per-step entity movement into the
ring, the game data plane's publish-subscribe pattern.

The asset baker plane plus the OpenUSD stage tier form weft's **asset CDN**: the
baker bakes a source glb Character into a content-addressed OpenUSD stage
(request-response), and the stage tier caches and distributes baked stages to clients
like a CDN. Baking
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

## Clients: desktop and VR

Players join from one Godot client (`fabric-godot-core`) that runs in two display and
input modes, not two separate clients: a **desktop** mode (flat window, keyboard and
mouse) and a **VR** mode (a SteamVR headset with OpenXR). Both control an avatar in the
same world, both connect to the Fly-hosted gateway over WebTransport, so a player can
play on desktop or in a headset. Desktop is the easier mode to iterate and test; VR is
the priority experience.

The rest of this section works through the VR mode, which is the harder case. The
desktop mode is the same client with input from keyboard and mouse instead of a
tracker, and a flat window instead of a headset. Two separations apply.

### Client, not a plane

A SteamVR HMD is a remote client across the network over WebTransport, not on
iceoryx2, so it is a client like Godot, not a plane. The word plane stays for
server-side iceoryx2 processes. The server-side parts that feed and consume the
headset's data are planes.

### Authority and interest, both

A HMD player is both an authority and an interest subscriber. This is the
authority/interest split, formalized in
[`lean-interest-mgmt`](https://github.com/v-sekai-multiplayer-fabric/lean-interest-mgmt).

- **Authority (upstream).** Each entity is authoritative on exactly one zone (the game
  data plane, proven single-owner), which advances its world and physics state. The
  exception is a player's avatar tracked pose, head and hands: that comes only from the
  HMD tracker, so the client is the source and sends it upstream. The zone owns
  everything else about the avatar.
- **Interest (downstream).** The player has interest in the surrounding world and
  receives read-only area-of-interest replicas, served as `CH_INTEREST` snapshots. A
  peer can hold interest in an entity without authority over it; interest replicas do
  not consume authority slots and are bounded separately, with causal vector-clock
  staleness and k-tick lookahead.

A pure observer is the degenerate case: interest only, zero authority. The "observe
from SteamVR" ask is this case; a full VR player adds the avatar pose authority
upstream.

### Transport

Both directions ride WebTransport. Live pose upstream and live world interest
downstream use unreliable drop-stale datagrams for the latest state (see `protocol.md`
and `latency.md`). Control and pulling baked OpenUSD stages from the asset CDN use
reliable streams.

### The interest feed is its own plane, always

The interest feed is an interest producer, never authority. It is one plane, used the
same way from one headset to a thousand or more. There is no scale threshold that
switches to a different path, because two modes are hard to QA and add a branch (see
the no-branching rule). The interest feed plane reads the game data plane's output and
produces `CH_INTEREST` for headsets on its own cores, so headset fanout never steals
cycles from the authority simulation, at any scale.

A wider or enriched interest view (a whole-zone or director camera, overlays beyond
per-peer area-of-interest culling such as everyone's names and stats or event markers)
and delay-and-replay are added as capabilities of this one plane, never as a separate
mode.

### Engine

The VR client is **Godot**, not a custom HMD client, consistent with "Godot stays on
the client." It uses Godot's OpenXR and SteamVR support and consumes WebTransport. The
engine build is
[`fabric-godot-core`](https://github.com/v-sekai-multiplayer-fabric/fabric-godot-core)
(the Godot fork), tagged by
[`fabric-godot-assembly`](https://github.com/v-sekai-multiplayer-fabric/fabric-godot-assembly)
(the multiplayer-fabric merge/assembly). Prebuilt binaries, the editor and the export
templates for Linux and Windows, come from
[`godot-images`](https://github.com/v-sekai-multiplayer-fabric/godot-images) release
assets, so the client needs no source compile of Godot.

### Desktop mode

The same Godot client runs on the desktop in a flat window. The player controls their
avatar with keyboard and mouse, so the avatar pose comes from input, not a tracker;
the zone holds authority over that avatar like any entity. It receives the same
interest feed (`CH_INTEREST`) and pulls the same world from the asset CDN. Desktop mode
is the easier mode to run, so it is the main way we get QA data on the live pipeline
before putting a headset on.

### Deployment: Fly.io

weft and its planes run on Fly.io. The container image holds the plane binaries at
fixed paths. Both client modes connect to the Fly-hosted gateway over WebTransport, so
a player joins the same world from desktop or a headset. Running both against the same
Fly deployment gives comparable QA data across the two modes.
