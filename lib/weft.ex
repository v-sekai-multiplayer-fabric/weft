defmodule Weft do
  @moduledoc ~S"""
  The main rule of weft's architecture:

  > **The BEAM runs only the control plane. Every other plane is a native process
  > outside the BEAM. The BEAM reaches it through Eclipse iceoryx (zero-copy IPC).**
  >
  > **A plane has no networking. An edge is a plane with networking.**

  > **What is built.** The control plane runs. The ring, the store, the interest
  > producer, and the SUMO producer run in the BEAM. **No plane and no edge exists yet.**
  > `native/dataplane` links only threads.
  >
  > The bus is the one part that changed. `native/harness` passes a message between two
  > processes over iceoryx2, checked at the far end, with no daemon. Nothing calls it yet,
  > so it proves the bus and not a plane. See `../native/harness/README.md`.
  >
  > So this page describes the design that the code is written toward, not a running
  > system. Each part carries its own state beside its code, and
  > `docs/essays/yagni.md` for why the order is what it is.

  The BEAM does coordination: placement, lifecycle, supervision, routing,
  backpressure, and caching. It is good at coordination and bad at heavy compute.
  So no heavy compute runs in it. Any work that parses, simulates, crunches, or links
  a large C++ library runs as its own native process. We call each one a **plane**.

  ## Terms

  One name per concept. The character terms are the Khronos 3D Formats Characters and
  Avatars TSG definitions (see `CITATION.cff`); the runtime terms are weft's.

  Runtime:

  - **plane**: a native process with no networking. It reaches other processes only
    through iceoryx. Networking is off (`--unshare-net`). There is no exception.
  - **edge**: a plane with networking. It obeys every plane rule below, and it adds one
    capability, the network. It terminates a transport and gives the decoded result to a
    plane or to the control plane over iceoryx. An edge holds no authority, runs no
    simulation, and keeps no durable state.
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

  1. **It is a separate native OS process.** The container image starts it. The BEAM
     does not start it as a Port.
  2. **It is sandboxed and crash-isolated.** Each plane runs in a
     [bubblewrap](https://github.com/containers/bubblewrap) sandbox: restricted
     filesystem, namespaces, and seccomp. It can reach only what it is given — the
     iceoryx segment, its own binary, and its data — not the host, the BEAM, or other
     planes. Because planes talk over iceoryx, not the network, **a plane has no
     networking** (`--unshare-net`). This is a definition, not a default, so there is no
     exception to check. A process that needs the network is an edge, not a plane. This
     matters for a plane that handles untrusted input: the asset baker parses arbitrary
     glb files, so with no network a broken or
     exploited baker cannot reach out. It is also
     crash-isolated: if it crashes, the BEAM keeps running and the plane is restarted
     on its own. The BEAM checks liveness only. In a container host this is a second
     layer inside the container of the machine.
  3. **It talks to the BEAM only through Eclipse iceoryx** (zero-copy IPC). Never a
     Port. Never a socket on the hot path. Two iceoryx patterns cover all cases:
     - **Publish-subscribe** for streaming state. The plane publishes samples; the
       BEAM subscribes and takes the latest.
     - **Request-response** for jobs. The BEAM sends a request; the plane responds.
  4. **The BEAM side is a small iceoryx NIF.** It takes a sample or sends a request,
     copies the bytes into a BEAM binary, and returns at once. No long work, no
     busy-poll, no blocked scheduler. The hard rules in `Weft.DataPlane` apply to
     every plane.
  5. **A plane is a thread-per-core process over iceoryx2.** Every plane runs a thin
     C++ harness. The harness pins one thread per core and runs a poll loop on an
     iceoryx `WaitSet`. iceoryx2 is brokerless, so no daemon runs beside a plane. weft
     writes no Rust, so the harness is C++ over iceoryx2's C++ bindings. iceoryx2 itself
     is Rust, and it is a dependency and not weft code. There is one runtime model for
     all planes, not a choice per
     plane. A CPU-bound plane busy-polls. An I/O-bound plane runs blocking worker threads
     over the same iceoryx transport. See `../essays/runtime-choice.md` for the measured basis.
  6. **The control plane orchestrates.** It decides where a plane runs, its lifecycle,
     backpressure, and result caching. The BEAM owns _what_ and _where_. The plane
     owns _how fast_.

  ## Planes today

  | Plane       | Native stack                                       | iceoryx pattern   | Isolation                   |
  | ----------- | -------------------------------------------------- | ----------------- | --------------------------- |
  | Control     | BEAM (weft)                                        | —                 | supervised (OTP)            |
  | Game data   | thread-per-core C++ harness + Jolt                 | publish-subscribe | out of BEAM                 |
  | SUMO        | Eclipse SUMO traffic microsimulation               | publish-subscribe | out of BEAM                 |
  | Interest    | thread-per-core C++ harness                        | publish-subscribe | out of BEAM                 |
  | Asset baker | OpenUSD + Adobe glTF plugin (fabric-stage-runtime) | request-response  | out of BEAM, crash-isolated |
  | Godot       | fabric-godot-core, headless                        | publish-subscribe | out of BEAM, crash-isolated |
  | Store       | native SQLite (WAL) + FoundationDB replica         | request-response  | out of BEAM, crash-isolated |

  No row above has networking.

  ## Edges today

  An edge is a plane with networking. It follows the plane contract above without change:
  a separate native process, sandboxed, crash-isolated, thread-per-core, and reached over
  iceoryx. The one difference is that the sandbox keeps the network.

  An edge terminates a transport, decodes the wire format, and gives the result to a plane
  or to the control plane. It holds no authority, runs no simulation, and keeps no durable
  state. So the network stays at the edge, and the work stays in the planes behind it.

  | Edge    | Native stack   | Terminates             | Gives the result to |
  | ------- | -------------- | ---------------------- | ------------------- |
  | Ingest  | picoquic + C++ | player input datagrams | the game data plane |
  | Gateway | picoquic + C++ | client control streams | the control plane   |

  The ingest edge carries the unreliable datagrams: player input upstream, and the
  interest plane's `CH_INTEREST` snapshots downstream. The gateway edge carries the
  reliable, low-rate work: login, chat, control, and asset pulls. `Weft.Gateway` stays in
  the control plane as the transport-agnostic routing core. The gateway edge gives it
  decoded requests, so the BEAM still touches no socket.

  A client holds one session to each edge. This costs two handshakes and two congestion
  controllers on one link. We accept that cost to keep the datagram path and the control
  path in separate processes, so control work cannot delay the datagrams.

  The SUMO plane is the current focus. It streams per-step entity movement into the
  ring, the game data plane's publish-subscribe pattern.

  The asset baker plane plus the OpenUSD stage tier form weft's **asset CDN**: the
  baker bakes a source glb Character into an OpenUSD stage
  (request-response), and the stage tier caches and distributes baked stages to clients
  like a CDN. Baking
  is off the game hot path. See `../essays/runtime-choice.md`.

  ## The bake machine

  A bake is slow, large, and the same every time. That inverts each rule the hot path
  obeys, so a bake does not run in a world machine. It runs on its own machine, and that
  machine scales on its own.

  |               | hot path                 | bake                   |
  | ------------- | ------------------------ | ---------------------- |
  | budget        | 16.67 ms                 | seconds to minutes     |
  | result        | 20 bytes for each entity | megabytes to gigabytes |
  | repeated work | each frame               | one time, forever      |

  `bake(source, tool version)` gives the same result every time, so weft addresses the
  result by content. The key is `hash(source)` with the tool version. A hit costs no
  compute and no upload.

  Zero copy is not why the bake machine uses iceoryx. One copy of a 200 MB result costs
  near 20 ms at 10 GB/s. That is 0.07 percent of a 30 s bake. The reason is the sandbox. A
  baker parses a file from a person we do not trust. So the baker must not have the
  network:

  ```
  bake machine
  ├── fetcher edge   has the network. It gets the source and writes the chunks.
  └── baker plane    has no network. It parses the source and converts it.
  ```

  The edge does each read and each write. The plane does each parse. iceoryx carries the
  result between them, and the parser never opens a socket.

  FoundationDB holds the chunks and the manifest, not the artifact. A value has a 100 kB
  limit, and a transaction has a 10 MB and 5 s limit. So the edge cuts the artifact into
  casync chunks near 64 kB, which fit a value, and writes many transactions. See
  `Weft.Actor.Store`.

  ### How a person uses a bake

  1. The client sends the hash to the gateway edge, on a reliable stream.
  2. The control plane reads the manifest for that hash from FoundationDB.
     - A hit returns the manifest. There is no upload and no bake. This is the usual case.
     - A miss takes the upload, then starts a bake.
  3. The bake machine gets the source, converts it, cuts it into chunks, and writes the
     manifest.
  4. The control plane tells the client that the asset is ready.

  A bake starts only when a person asks for the asset. weft does not bake ahead of the
  request. The first person to ask waits. An eager bake spends compute on an asset that
  nobody wears.

  ### The world machine does not carry an asset

  An avatar of 20 MB, sent to 100 persons who join across 60 s, is 267 Mbit each second.
  The interest fanout needs 0.37 Gbit each second, so the asset would take most of a
  1 Gbit link and starve the work the world machine exists to do.

  So the interest snapshot carries the hash, which is 32 bytes. The client then gets the
  chunks from the asset CDN, on a different connection to different machines. A world
  machine never sends a byte of an asset.

  Chunks are addressed by content, so the S3 tier is one tier for every region, and the
  FoundationDB of each region is a cache in front of it. weft runs in one region today, so
  a second region gets the same bakes with no new work. See `../essays/topology.md`.

  New planes (physics, ML inference, video/audio transcode) use the same contract: a
  native process plus iceoryx publish-subscribe or request-response. There is nothing
  new to design per plane.

  The thread-per-core harness is the loop every plane runs, not a plane by itself. In
  the game data plane, the harness runs the loop, drives Jolt (physics), and reaches the
  control plane and other planes through iceoryx. Every plane uses the harness the same
  way. The harness is a thin C++ layer over iceoryx2, not Seastar and not Rust. Linux
  is the primary target. Windows support in iceoryx2 is experimental. See
  `../essays/runtime-choice.md` for the measured basis.

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

  ### Who holds the FoundationDB client

  Two processes hold it, and only two. The control plane holds `erlfdb`. The store plane
  holds `libfdb_c`. No other plane links a FoundationDB client.

  A plane that needs durable state asks the store plane over iceoryx, request-response.
  The store plane answers from SQLite, and SQLite reads its pages through the VFS. So a
  plane sees a database and not a key range.

  The rule exists because the second client is never only a client. It repeats the API
  version, the network thread, `fdb_setup_network`, `fdb_run_network`, and the
  `fdb_transaction_on_error` retry loop. Each copy then chooses its own key layout, its own
  transaction size, and its own answer to error 1020. Those choices drift, and a reader
  cannot tell which copy is right.

  This does not make the store plane a networked plane. A plane has no networking, which
  means it terminates no transport and accepts no connection. FoundationDB sits below the
  planes, and reaching down to it is not the same as facing a client.

  **Not held today.** `native/gyreplane` links its own `libfdb_c` in `src/fdb_database.c`,
  with its own key layout in `src/zf_kv.c`. It cannot stop until the store plane has an
  iceoryx harness. See `../native/gyreplane/WEFT.md`.

  ## Results

  - The BEAM stays fast. It never does slow work, so scheduler latency stays low no
    matter what the planes do.
  - Each plane can use C++ and its own CPU
    cores.
  - A plane can be replaced, scaled, or crash without touching the control plane.
  - Deployment is the same for all planes: the container image holds the plane
    binaries at fixed paths, so shared-library paths are known. A container host runs
    that image, chosen later.

  ## Clients: HMD, desktop, TUI

  Players and QA join from one Godot client (`fabric-godot-core`) that runs in three
  display modes, not three separate clients:

  - **HMD**: a SteamVR headset with OpenXR. The priority experience.
  - **desktop**: a flat window with keyboard and mouse. Local play and QA.
  - **TUI**: a headless ASCII terminal. It needs no display and no GPU, so it runs on
    GitHub Actions for automated QA. It prints the entity grid each tick and checks the
    pipeline. See the headless-godot-tui-observer method.

  All three modes control an avatar in the same world and connect to the gateway over
  WebTransport. The rest of this section works through the HMD mode, the harder case. The
  desktop mode swaps the tracker for keyboard and mouse, and the headset for a window.
  The TUI mode swaps the render for printed text. Two separations apply.

  ### The client is a plane

  A Godot client is a plane. It is a native process outside the BEAM that does heavy work,
  which is the whole definition. It reaches the rest of weft over iceoryx the same way any
  plane does.

  The headset is not the plane. A SteamVR HMD is a device on the far side of a
  WebTransport session, and that session ends at an edge. What runs on the machine is a
  Godot process, and that process is a plane.

  This matters for the TUI mode most. It needs no display and no GPU, so it runs on GitHub
  Actions, which means weft hosts it rather than shipping it. A hosted Godot process with
  no networking of its own is a plane by every rule in this page.

  ### Authority and interest, both

  A HMD player is both an authority and an interest subscriber. This is the
  authority/interest split, formalized in
  [`lean-interest-mgmt`](https://github.com/v-sekai-multiplayer-fabric/lean-interest-mgmt).

  - **Authority (upstream).** The Terms above give the rule. The exception is a player's
    avatar tracked pose, head and hands: that comes only from the HMD tracker, so the
    client is the source and sends it upstream. The zone owns everything else about the
    avatar.
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
  downstream use unreliable drop-stale datagrams for the latest state (see `Weft.Gateway`
  and `../essays/latency.md`). Control and pulling baked OpenUSD stages from the asset CDN use
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

  ### Build, test, release, and QA: GitHub Actions

  The whole system builds, tests, releases, and runs QA in GitHub Actions. A release
  follows RFD 0067 (dev, then beta, then rc): Elixir apps as OTP releases, the
  Godot client packaged, then fpm RPMs, and desync chunks pushed to the casync store.
  Automated QA runs the Godot client in TUI mode, which needs no display, so it runs on a
  CI worker. The runtime host is deferred.
  """
end
