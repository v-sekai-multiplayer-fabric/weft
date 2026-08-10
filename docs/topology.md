# Topology

How many machines, where they are, and what runs on each one. Every number below comes
from `benchmarks.md` or from arithmetic on it. Where a number is a guess, it says so.

## Latency does not set the geography

A first version of this page put the network inside the motion-to-photon budget and got
a radius of 400 km. That was wrong.

Motion sickness comes from the delay between your head movement and your own view. The
headset renders your own view on the client with prediction, so that path is local. It
does not cross the network. The network delay changes how smoothly you see **other** people move. Interpolation
hides that delay up to about 80 ms.

So the budget is:

| Path | Budget | Set by |
| --- | --- | --- |
| your head to your own view | 20 ms | the client, local, no network |
| other persons to your view | about 80 ms | interpolation quality |

At 9.8 microseconds for each kilometre of fibre, round trip, 80 ms gives a radius near
4000 km.

## One region

weft runs in one region. Start with `sjc`, on the west coast of North America.

A radius of 4000 km holds the 80 ms budget, so one region does not cover the world. This
is what one region gives:

| From | Round trip | Result |
| --- | --- | --- |
| west North America | below 30 ms | good |
| east North America | near 70 ms | good |
| Japan | near 110 ms | avatars stutter |
| Europe | near 150 ms | avatars stutter |

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

### Zones: one is enough

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

For 1000 avatars at 5 m/s in a world 2 km wide, `sqrt(Z) >= 0.21`, so **Z = 1**. One zone
is sufficient, with a margin near 20 times. Do not build spatial partitioning yet. See
`yagni.md`.

### What binds: bytes to the client

Each client receives `K` entities of `b` bytes at `f` Hz. Area-of-interest culling gives
K = 256. Compression gives b = 3 bytes, from 20 bytes at the measured 6.5 times. The rate
is f = 60 Hz. One client then costs 46 kB each second, which is 0.37 Mbit each second.

```
clients_max = B / (K b f 8)

  1 Gbit/s ->  2712 clients
 10 Gbit/s -> 27000 clients
```

### Machines hold many worlds

Compute does not bind. 1000 entities at 60 Hz is 60000 applies each second, against a
measured 41 million for each core. That is 0.15 percent of one core. State is 20 kB.

```
worlds_for_each_machine = B / (players K b f 8)

 10 Gbit/s, worlds of 1000 -> 27 worlds, near 27000 persons for each machine
```

So a bigger machine does not help a world. More machines hold more worlds. This is the
horizontal direction: a world is the unit, and worlds are independent.

## Above the crossover

Past `B/(K b f 8)` clients, one machine cannot send to all of them. That is near 2712 on
1 Gbit/s. The interest plane reads the ring in the authority machine. A second machine has no
access to that memory. So the authority machine must send the state to interest replicas over
the network.

This is the one path where a plane on one machine sends to a plane on a different
machine. It is one way, and it is small: 1000 entities of 20 bytes at 60 Hz is 1.2 MB
each second. Build it when the client count passes the crossover, and not before.

## High availability

| Tier | Count | Why |
| --- | --- | --- |
| FoundationDB | 3 machines | `triple` redundancy keeps 3 copies and accepts 2 losses. Coordinators must be an odd number: `n = 2f+1` accepts `f` losses. |
| front door (login, matchmaking, directory) | 2 or 3 machines | It holds no world state, so it replicates freely. Use 3 for a rolling update with no loss of capacity. |
| world machine | 1 | An entity is authoritative on exactly one zone. Two live copies of a world are two writers. |

A world machine cannot be two. That is the single-writer rule, not a limit of the deploy.
When a world machine is lost, the persons in it are disconnected and they join again. The
world state rebuilds from FoundationDB, a measured 0.448 ms read. A social 3D platform
usually accepts the same loss, because a world instance is short-lived.

FoundationDB is strongly consistent and wants a short distance between its processes. So
each region gets its own FoundationDB cluster. One cluster does not span two regions.

## Global data: one authoritative world

Identity, friends, and matchmaking are the same in every region. A FoundationDB cluster
for each region cannot hold them.

The answer needs no new mechanism. One world is authoritative for this data, and the data
lives there as actors. weft already has the rule: an actor has a single writer, and it
lives in one place. Identity is that rule at a different level.

So there is one home world. An account is an actor in it. A friend list is an actor in
it. The store tiers it the same way as any other actor: a local SQLite primary, an async
FoundationDB replica, and the S3 cold tier. Handoff, restore, and the limits all work
without a change.

| Data | Home | Read from a different region |
| --- | --- | --- |
| account, friends, matchmaking | the home world | over the network, then cached |
| assets | no home, addressed by content | the S3 tier serves every region |
| world state | the world machine | it does not leave the region |

With one region today, the home world is in that region and nothing crosses a network.
A second region reads the home world across the network. A read is near 150 ms from
Europe to west North America. That is acceptable, because a person reads a friend list at
login and not each frame. A write goes to the home world, so there is one writer and no
merge.

The home world is a single point of failure for login. It has the same answer as any
world machine: the state rebuilds from FoundationDB, a measured 0.448 ms read. A person
already in a world keeps playing, because world state does not need the home world.

### One name for one concept

`planes.md` says an entity is a simulated thing with position and velocity, so an account
is not an entity. An account is an **actor**, which is the single-writer primitive. Use
the actor name for this data.
