# Why is a multiplayer fabric benchmarked on traffic?

weft moves avatars. Its one real workload is a traffic simulation, which sounds like a
category error until you try to get a real one any other way.

A synthetic benchmark writes random entities at random positions. It measures the machine
and nothing else, because real movement is not random. Entities near each other move
together, they stay near each other for a while, and the bytes that describe them compress
against the frame before. A random benchmark destroys all three properties and then reports
a number that no real workload will ever reproduce.

So weft needed real movement, and getting it usually means shipping a game first. SUMO is
the shortcut. It is a traffic microsimulation, it is free, and a city of vehicles is
thousands of entities moving coherently for as long as you care to run it.

`../logbook/data_plane.md` holds what that bought: 11947 distinct vehicles, 8637 of them
moving at once, and 2950620 entity updates across 600 frames. The synthetic bench said one
core does 826 M applies each second. The trace said 840 M. The synthetic number survived
contact with real data, which is the only reason to trust it now.

## The surprise is which case is hard

A vehicle is the easy entity, and that is not obvious until you look at what one does.

It moves on rails. SUMO's road network is one and a half dimensions: a lane, a position
along it, and the occasional lane change. Two vehicles never occupy the same point,
because the model forbids it before physics gets a chance to care. Density has a ceiling
built into the road.

A crowd has none of that. People in a plaza are a continuous two-dimensional problem, and
what makes it hard is exactly what the road model removes. Bodies touch. Pressure builds
at a doorway. An arch forms across a bottleneck and holds, and then it collapses, and the
flow through that gap is not something you can derive from how many people wanted to go
through it.

That is the multiplayer case. A hundred avatars in a venue is not a traffic jam, and it is
the shape weft exists for.

## What the traffic number does not prove

Put the trace beside the machine and the gap is embarrassing.

The trace is 4918 entity updates in each frame. One core, bounded by memory bandwidth
rather than by anything weft wrote, handles about 1493 worlds that size. The traffic
workload does not stress weft. It never came close.

So the trace proves the pipeline, and not the load. It shows that the decode path, the
ring, and the wire format all work on movement that a real simulator produced. It says
nothing about what happens when the entities are dense enough to touch.

It also settles the wire format question, which is the other thing it was for. The
bitpacked format is 12 bytes for each entity and the CBOR one is 28, which is 2.3 times.
After compression against the previous frame the gap falls to 1.4 times, because the
repeated field names compress away. That is a real result and it needed real frames.

## Coupling the hard case, and what it costs

SUMO can hand its pedestrians to JuPedSim, which models them as bodies in continuous space
with contact forces rather than as positions on a stripe. `../../test/bench/sumo/README.md`
holds the configuration.

The cost is not small, and it is worth naming before anyone reaches for it.

It is a second simulation engine, with its own model and its own parameters. The walkable
areas are polygons somebody draws by hand in an editor, and every one of them needs entry
and exit edges stitched to the road network so an agent can cross between the two spatial
models. The step length drops to 0.05 s, because contact forces need it, and that is
twenty steps for every second of simulated time.

None of that is weft's code, and all of it is weft's problem the moment the trace matters.

So the honest position is that weft has one real workload, it is the easy one, and it was
worth having anyway. The hard one is available and unbuilt. That order is deliberate:
`yagni.md` argues it, and a benchmark that took a week to author would have proved less
than the one that took an afternoon.
