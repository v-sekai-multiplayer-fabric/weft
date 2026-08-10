# YAGNI

Reference:
[RFD 0071, yagni times structure to need](https://v-sekai-multiplayer-fabric.github.io/multiplayer-fabric-manuals/rfd/0071-yagni-times-structure-to-need/index.html).

Most people hear YAGNI as advice about thrift: do not build the expensive thing. That
reading is wrong, and getting it wrong produces a codebase full of clever deferrals
that cost more than the thing they avoided.

YAGNI is about **timing**, not cost. Structure built ahead of the feature that needs it
spends an option early and delays a return. The same structure built when the need
arrives costs the same and earns immediately. Nothing is saved by never building it.
Something is lost by building it too soon.

So the question is never "is this expensive?" It is "does a real need arrive soon?"

## The pass, and the surprise in it

We ran that question over everything weft had designed but not built. The expectation
going in was that a list of speculative items would fall out.

Almost nothing did.

The reason turned out to be a property of the product rather than discipline on our
part. Every item that looked speculative in isolation had the same single consumer:

> **Play and observe the SUMO world from a Godot client, on HMD, desktop, and TUI. The
> whole system builds, tests, and releases in GitHub Actions. VR is the priority
> experience. TUI is automated CI QA. The runtime host is deferred.**

Once that sentence exists, the pieces stop looking like a platform someone might want
and start looking like a chain, where removing any link breaks the demo:

- The **SUMO game data plane** plays the traffic simulation. That is the world.
- The **asset CDN** stores it and delivers it to the client.
- The **interest feed** serves the headset its area-of-interest replicas
  (`CH_INTEREST`).
- The **gateway** routes the headset to its zone.
- The **Godot client** observes it, in VR, on the desktop, or as text in CI.
- The **store** holds the durable state underneath.

That is the useful outcome of a YAGNI pass, and it is not the outcome we expected.
It did not produce a list of things to cut. It produced evidence that the design has
one consumer rather than several imagined ones. A design serving one real consumer is
load-bearing. A design serving several imagined consumers is a platform, and a
platform is the thing YAGNI is warning you about.

## Where it did bite

Two places, and both are worth naming because they are the ones we could have got
wrong.

**Two modes for interest.** It is tempting to serve one headset with a simple path and
switch to a different path above some threshold. That is a scale optimization ahead of
scale, and worse, it doubles what QA must cover forever. So the interest feed is one
plane at every scale, from one headset to a thousand. There is no threshold and no
second mode. `planes.md` calls this the no-branching rule.

**Spatial partitioning.** The maths says one zone holds the target with a margin near
20 times. See `topology.md`. Building zone splitting now would be structure with no
need behind it.

## Compaction is not optional, which is a different thing

Compaction folds DELTA rows into SHARD. It looks like an optimization and it is not.
Without it the log grows without bound and the system stops on resources.

Worth separating clearly: YAGNI defers structure that a future need might justify. It
never defers the thing that makes the current design terminate. "We will add cleanup
later" is not YAGNI, it is a bug with a schedule.

## The proof is the product, not a benchmark

A transport microbenchmark proves that a transport is fast. It does not prove that
anybody can be present in a shared world. The proof we care about is the product
question:

> **Can we carry our players without motion sickness, with presence, at scale?**

- **No motion sickness.** Head and hand pose render locally at headset rate with
  reprojection, so the network never gates motion-to-photon.
- **Presence.** World state stays fresh, with no rubber-banding and no stale pops.
- **Scale.** Many entities and many headsets at once.

The SUMO world in VR is the testbed for that proof.

## Documenting is not building

The plane and CDN designs stay written down even where the code does not exist.

Writing a design is cheap, and it keeps the road not taken visible so the next person
does not rediscover it. YAGNI bites on building the structure, not on describing it.
This page is itself an instance of the rule.
