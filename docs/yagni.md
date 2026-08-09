# YAGNI pass

Reference:
[RFD 0071, yagni times structure to need](https://v-sekai-multiplayer-fabric.github.io/multiplayer-fabric-manuals/rfd/0071-yagni-times-structure-to-need/index.html).
YAGNI is a timing rule, not a thrift rule: building structure ahead of the feature
that needs it spends an option and delays a return. Build structure only when a real
near-term need arrives.

## Result: the pass converged

The items that looked like structure ahead of need are one coherent near-term
product with one consumer, so they are load-bearing, not speculative:

> **Play and observe the SUMO world from a Godot client, on HMD, desktop, and TUI.
> The whole system builds, tests, and releases in GitHub Actions. VR is the priority
> experience. TUI is automated CI QA. The runtime host is deferred.**

Data flow:

- The **SUMO game data plane** plays back the traffic simulation. Its entity movement
  is the world we observe. Playback of the SUMO data is a game data plane.
- The **asset CDN** stores the SUMO simulation (world stage plus recorded data) and
  delivers the world to the client.
- The **interest feed** serves the headset its area-of-interest replicas
  (`CH_INTEREST`), per the authority/interest split.
- The **gateway** routes the headset to its zone over WebTransport.
- The **Godot VR headset client** observes: drop-stale datagrams for live world state,
  reliable streams for pulling the stored world from the asset CDN.
- The **store** holds durable control-plane state, with compaction so it does not grow
  without bound.

## Build now

VR first. All of these serve the product above.

- **Godot client**, desktop and VR modes (one client, two display and input paths).
  Desktop is local QA. VR is the priority experience. TUI runs on GitHub Actions for
  automated QA.
- **SUMO game data plane** (live playback into the ring). The observed world.
- **Interest feed** (`CH_INTEREST` to the headset). One plane, one path from one
  headset to a thousand or more. No scale threshold and no second mode, which are hard
  to QA.
- **Asset CDN** (stores the SUMO simulation; delivers the world to the client).
- **Gateway** (routes the headset to its zone).
- **Store with compaction.** Compaction (DELTA to SHARD) is required, not optional:
  without cleanup the DELTA rows grow without bound and the system halts on
  resources.

## The proof

The proof is not an abstract transport microbenchmark. It is the product working:

> **Can we carry our players without motion sickness, with presence, at scale?**

- **No motion sickness.** Head and hand pose render locally at headset rate with
  reprojection, so the network never gates motion-to-photon. The network carries world
  state; drop-stale datagrams keep it fresh so it never stalls or judders.
- **Presence.** World state stays fresh and stable, no rubber-banding, no stale pops.
- **Scale.** Many entities and many headsets.

The SUMO-in-VR pipeline is the testbed for this proof.

## Nothing is gated

The pass converged fully. There is no gated item and no open question. The interest
feed is one plane at any scale, so there is no two-mode spectator path to defer. The
1000-headset requirement sets the single design; we always use it.

## Note on the docs

The plane and CDN designs stay documented. Documenting a design is cheap and keeps
the road not taken. The rule bites on building the structure, not on describing it.
