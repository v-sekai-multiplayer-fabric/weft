# Topology

How many machines, where they are, and what runs on each one. Every number below comes
from `benchmarks.md` or from arithmetic on it. Where a number is a guess, it says so.

## The mistake that shaped this page

The first version of this page reasoned like this: VR needs motion-to-photon under
20 ms, the network sits inside that budget, so players must be within 400 km of a
datacenter. That produced a plan for many small regional sites.

It was wrong, and the error is worth keeping visible because it is easy to make.

Motion sickness comes from the delay between moving your head and seeing your own view
change. Your headset renders your own view locally, with prediction. The network is not
in that loop at all. What the network affects is how smoothly you see _other_ people
move, and interpolation covers that up to roughly 80 ms.

Two different budgets had been collapsed into one:

| Path                       | Budget      | Set by                        |
| -------------------------- | ----------- | ----------------------------- |
| your head to your own view | 20 ms       | the client, local, no network |
| other persons to your view | about 80 ms | interpolation quality         |

At 9.8 microseconds for each kilometre of fibre, round trip, 80 ms gives a radius near
4000 km.

## One region

weft runs in one region. Start with `sjc`, on the west coast of North America.

A radius of 4000 km holds the 80 ms budget, so one region does not cover the world. This
is what one region gives:

| From               | Round trip  | Result          |
| ------------------ | ----------- | --------------- |
| west North America | below 30 ms | good            |
| east North America | near 70 ms  | good            |
| Japan              | near 110 ms | avatars stutter |
| Europe             | near 150 ms | avatars stutter |

A distant person can play, and the world stays correct. An entity is authoritative on
exactly one machine, wherever the person sits. Only the smoothness of other avatars gets
worse.

One region also holds the fixed cost down. FoundationDB needs 3 machines for `triple`
redundancy in each region. Those machines run whether a person is online or not. A second
region doubles that floor before it serves one person.

Add a region when the persons are there, not before. Each new region is one more
FoundationDB cluster and one more front door. A world still does not span two regions.
This is the usual design for a social 3D platform.

## A world does not cross a machine

iceoryx does not cross machines, so all of one world stays on one machine. That machine
runs the control plane, the game data plane, the interest plane, the store plane, and the
edges.

This is not one world for each machine. A machine holds many worlds, because a world
costs little. See "Machines hold many worlds" below. The rule is only that a world does
not split.

A world could split, but the cost is a proof, not a configuration change. To hand an
entity across machines inside one frame, you need a predictive method. The n-frame
predictive BVH in the Lean repositories is one. Until that is in weft, treat a world as
one machine.

### How many zones does a world need?

Spatial partitioning is what everyone builds. It is worth checking whether this world
needs any.

Take N entities at speed v in a square zone of side L. The boundary crossing rate is
`R = 4Nv/(pi L)`. Divide a world of side W into Z zones. Each zone then gets:

```
R_z = 4 N v / (pi W sqrt(Z))
```

A cross-machine handoff costs about 0.65 ms: BEAM distribution near 0.2 ms, plus the
measured 0.448 ms FoundationDB read. Hold that below 1 percent of a 16.67 ms frame and
each machine gets 15 handoffs each second. Solve for Z:

```
sqrt(Z) >= 4 N v / (15 pi W)
```

For 1000 avatars at 5 m/s in a world 2 km wide, `sqrt(Z) >= 0.21`, so **Z = 1**.

One zone. Not one zone as a simplification to revisit later, but one zone with a margin
of about 20 times. The obvious feature turns out to be unnecessary by a wide margin, and
building it now would be structure with nothing behind it. See `yagni.md`.

### So what does bind?

Not compute, as it turns out, and not handoffs either. Bytes.

Each client receives `K` entities of `b` bytes at `f` Hz. Area-of-interest culling gives
K = 256. Compression gives b = 3 bytes, from 20 bytes at the measured 6.5 times. The rate
is f = 60 Hz. One client then costs 46 kB each second, which is 0.37 Mbit each second.

```
clients_max = B / (K b f 8)

  1 Gbit/s ->  2712 clients
 10 Gbit/s -> 27000 clients
```

### The direction that scales

1000 entities at 60 Hz is 60,000 applies each second, against a measured 41 million per
core. That is **0.15 percent of one core**, and 20 kB of state. A world barely registers
on the machine that runs it.

```
worlds_for_each_machine = B / (players K b f 8)

 10 Gbit/s, worlds of 1000 -> 27 worlds, near 27000 persons for each machine
```

Which settles the scaling question in an unusual way. A faster CPU buys nothing, because
nothing is CPU-bound. Vertical scaling is not merely inefficient here, it is inert. The
only direction that moves is sideways: more machines, more worlds, and worlds never talk
to each other.

## The one place the design breaks

Everything above holds because a world fits on a machine. Past `B/(K b f 8)` clients,
near 2712 on 1 Gbit/s, it stops fitting: one machine cannot send to that many people.

The awkward part is that you cannot solve it by adding a machine, at least not for free.
The interest plane reads the ring in shared memory, and a second machine has no access to
that memory. So the authority machine has to ship state over the network to interest
replicas.

That is a genuine exception to "planes never cross machines," and it is worth admitting
rather than hiding, because it is the seam where the architecture would have to change.
It is one way and it is small — 1000 entities of 20 bytes at 60 Hz is 1.2 MB each second
— so it is not hard. It is just not built, and it should not be built until the client
count actually crosses the line.

## High availability

| Tier                                       | Count           | Why                                                                                                                         |
| ------------------------------------------ | --------------- | --------------------------------------------------------------------------------------------------------------------------- |
| FoundationDB                               | 3 machines      | `triple` redundancy keeps 3 copies and accepts 2 losses. Coordinators must be an odd number: `n = 2f+1` accepts `f` losses. |
| front door (login, matchmaking, directory) | 2 or 3 machines | It holds no world state, so it replicates freely. Use 3 for a rolling update with no loss of capacity.                      |
| world machine                              | 1               | An entity is authoritative on exactly one zone. Two live copies of a world are two writers.                                 |

The last row is the one people argue with. Surely a world should have a hot standby?

It cannot. An entity is authoritative on exactly one zone, so two live copies of a world
are two writers, and two writers is not a degraded mode — it is a corrupted world. This
is not a limitation of the deployment that a better deployment would fix. It is the
single-writer rule, which is also the thing that makes the rest of the system simple.

So when a world machine is lost, the people in it are disconnected and rejoin. The state
rebuilds from FoundationDB in a measured 0.448 ms. A social 3D platform usually accepts
exactly this, because a world instance is short-lived anyway, and paying for warm
standbys to protect something that ends in an hour is a poor trade.

FoundationDB is strongly consistent and wants a short distance between its processes. So
each region gets its own FoundationDB cluster. One cluster does not span two regions.

## Global data: one authoritative world

Identity, friends, and matchmaking are the same in every region. A FoundationDB cluster
for each region cannot hold them.

The tempting move is to reach for a new mechanism: a global database, a replication
layer, an eventually-consistent merge with conflict resolution. Each is real work, and
each adds a concept the rest of the system has to know about.

None of it is needed, because the rule already exists. An actor has a single writer and
lives in one place. Identity is that same rule one level up: one world is authoritative
for the data, and the data lives there as actors.

So there is one home world. An account is an actor in it. A friend list is an actor in
it. The store tiers it the same way as any other actor: a local SQLite primary, an async
FoundationDB replica, and the S3 cold tier. Handoff, restore, and the limits all work
without a change.

| Data                          | Home                          | Read from a different region    |
| ----------------------------- | ----------------------------- | ------------------------------- |
| account, friends, matchmaking | the home world                | over the network, then cached   |
| assets                        | no home, addressed by content | the S3 tier serves every region |
| world state                   | the world machine             | it does not leave the region    |

With one region today, the home world is in that region and nothing crosses a network.
A second region reads the home world across the network. A read is near 150 ms from
Europe to west North America. That is acceptable, because a person reads a friend list at
login and not each frame. A write goes to the home world, so there is one writer and no
merge.

The home world is a single point of failure for login. It has the same answer as any
world machine: the state rebuilds from FoundationDB, a measured 0.448 ms read. A person
already in a world keeps playing, because world state does not need the home world.

### One name for one concept

`../reference/architecture.md` says an entity is a simulated thing with position and velocity, so an account
is not an entity. An account is an **actor**, which is the single-writer primitive. Use
the actor name for this data.
