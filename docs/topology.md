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

## Four regions

| Region | Covers |
| --- | --- |
| EU | Europe, Africa, west Asia |
| NA east | east North America, South America |
| NA west | west North America |
| Japan | east Asia, Oceania |

A world runs in one region. A person joins a world in a region, the same as VRChat. No
world spans two regions, so no state crosses a region on the hot path.

## A world does not cross a machine

iceoryx does not cross machines, so all of one world stays on one machine. That machine
runs the control plane, the game data plane, the interest plane, the store plane, and the
edges.

This is not one world for each machine. A machine holds many worlds, because a world
costs little. See "Machines hold many worlds" below. The rule is only that a world does
not split.

A world could split, but the cost is a proof, not a configuration change. Handing an
entity across machines inside one frame needs a predictive method, such as the n-frame
predictive BVH in the Lean repositories. Until that is in weft, treat a world as one
machine.

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
world state rebuilds from FoundationDB, a measured 0.448 ms read. VRChat accepts the same
loss.

FoundationDB is strongly consistent and wants a short distance between its processes. So
each region gets its own FoundationDB cluster. One cluster does not span EU and Japan.

## Open question: global data

Identity, friends, and matchmaking are the same in every region, and a FoundationDB
cluster for each region cannot hold them. Assets do not have this problem, because they
are content-addressed and the S3 tier is global. See `store.md`. Identity has no answer
yet. `tasks.md` records it.
