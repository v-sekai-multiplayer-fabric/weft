# The ingest edge

Terminates player input datagrams. Gives the result to the game data plane.

State: not started. This directory holds the contract and no code.

## What it carries

Unreliable datagrams, in both directions.

- **Upstream.** Player input. One packet is 24 bytes of movement.
- **Downstream.** The interest plane's `CH_INTEREST` snapshots.

Nothing here is reliable, and nothing here is ordered. A late packet is worth less than the
packet behind it, so a lost packet is dropped and not retransmitted.

## What it must not do

It holds no authority. Authority is the single writer of an entity, and that writer is in a
plane. This process decodes a datagram and passes it on.

It keeps no durable state. A datagram that arrives is decoded and published, and then it is
gone.

## The numbers it has to meet

`../../../docs/reference/data_plane_logbook.md` holds the measurements and the conditions
each one ran under. Two of them bound this edge.

The compute ceiling for decode and apply is 1.21 ns for each packet on one core, with the
packets and the entities in cache. Against a 2 GB entity table it is 24.2 ns. Real traffic
sits between those two.

Neither number includes the network card. The logbook records that gap as not measured, and
loopback cannot close it.

## What it needs first

1. **iceoryx v1 and RouDi**, to reach the game data plane. Not in the container image.
2. **The thread-per-core harness.** Every plane uses it, so it is built once.
3. **picoquic**, in `../transport`. That part is here already.
