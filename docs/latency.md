# Latency

Low latency is the first priority in weft. Each choice below is justified by
latency. Where a choice would trade latency for something else, we keep the
low-latency path and move the other work off the critical path.

## Control plane in the BEAM, heavy work in planes

The BEAM does only coordination, which is microsecond work. Heavy work runs in
native planes. A scheduler is never parked on slow work, so control-plane calls
stay in the low microseconds. A long NIF would park a scheduler for milliseconds;
we never do that.

## iceoryx for IPC

Planes talk over iceoryx (zero-copy). A message is a pointer, not a copy and not a
serialize step. This is the lowest-latency IPC available: sub-microsecond, no kernel
round trip, no socket. A socket or a Port would add copies and system calls.

## Small NIF pokes, nothing long on the hot path

The BEAM side of a plane is one small NIF call: take a sample or send a request,
then return. It never busy-polls and never runs long. The schedulers keep low
latency no matter what the planes do.

## Native data plane

Packet decode and physics run native (Seastar and Jolt), at nanoseconds per packet.
The BEAM never touches a packet. Putting packets in the BEAM would add a per-message
copy and garbage collection, which raises tail latency.

## Sample the latest, do not stream every update

The BEAM samples the latest state. It does not receive one message per update. That
is one copy at tick rate, not one copy per item, so ingest latency stays flat as the
update rate rises.

## Store: local fast writes, async durability

Actor state is written locally and fast (about a microsecond) and acked at once.
Durability and cross-machine handoff happen asynchronously, off the write path. We
do not write synchronously to FoundationDB on each change: that is about 1
millisecond, roughly 14 times the local write, and it would sit on the critical
path. FoundationDB holds ownership and receives async replication for handoff.
Accepted cost: a crash may lose the last few unreplicated writes.

## Transport: H3/WebTransport

Reliable control uses separate WebTransport streams, so one slow transfer does not
block another. Real-time updates use datagrams with an app sequence number, so a
late packet is dropped, not queued. Both cut tail latency.

## Sandbox cost is one-time

bubblewrap and networking-off add process setup cost once, at start, not per call.
They do not touch the hot path.
