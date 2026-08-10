# Latency

Every distributed system says it cares about latency. The question worth asking is
sharper: when latency conflicts with something else you want, which one loses?

In weft, the other thing loses. That sounds like a slogan until you follow it into the
places where it hurts, which is where this page goes.

## The BEAM is the wrong place for work, and that is why we use it

Start with an apparent contradiction. weft is an Elixir project, and Elixir runs the
whole control plane. Yet the rule is that no real work happens there.

The reason is that the BEAM's scheduler is cooperative. A process that runs long does
not just take time itself, it delays every other process queued behind it on that
scheduler. Coordination work — deciding which node owns a zone, restarting a crashed
thing, looking up an address — takes microseconds, and microseconds do not disturb
anyone. A physics step or a glb parse takes milliseconds, and milliseconds do.

So the BEAM earns its place by being excellent at coordinating, and coordination is the
only thing we let it do. A long NIF
would park a scheduler for milliseconds, so we never write one.

This has a consequence people find surprising: making the BEAM do less is what keeps
it fast. Every plane we move out is not a workaround for the BEAM being slow. It is
what preserves the property we wanted from the BEAM.

## The cheapest message is the one you never copy

Once heavy work lives outside the BEAM, those processes have to talk. The obvious
answer is a socket, or a Port, and the obvious answer is wrong by three orders of
magnitude.

A socket copies the bytes, crosses into the kernel, and comes back. iceoryx hands over
a pointer into shared memory. Nothing is copied and nothing is serialized, so the cost
is sub-microsecond rather than tens of microseconds.

That single decision then constrains the whole deployment, which is the part worth
noticing. Shared memory does not cross a machine. So planes cannot be spread across
machines, a world cannot be split across machines, and a Kubernetes Pod cannot help,
because the memory is what binds them. `topology.md` follows that thread. A transport
choice turned into a deployment architecture, and it is worth being honest that we
took the constraint on knowingly.

## Do not deliver every update. Deliver the newest one

The naive design sends the BEAM one message per state update. It is correct, it is
simple, and it caps out near 1.38M snapshots per second, because each message copies a
full term into a mailbox.

The alternative is a shared slot. The worker overwrites it, the BEAM reads whatever is
there. Measured: 2.85M per second on one core, 27.7M on sixteen.

The interesting part is not the speed, it is what you give up. Reads can miss updates
entirely. A reader that falls behind never catches up on what it missed — it just sees
the newest value.

For a live world this is not a loss, it is the correct semantics. An avatar's position
from three frames ago has no value to anybody. Nobody wants a queue of stale
positions delivered reliably. The naive design was doing extra work to preserve
something worthless, and dropping that work made it 20 times faster.

Sampling costs about 3 microseconds, so reading at 60 Hz is free.

## Durability belongs off the write path

A write to local SQLite takes about 70 microseconds. A synchronous FoundationDB
transaction takes about 1 millisecond, roughly 14 times more.

If durability sat on the write path, every actor write would pay that millisecond. So
it does not. A write acks locally, and replication to FoundationDB happens afterwards,
off the path.

That is the Elixir prototype. The plane arrives at the same place by a different road, and
the difference is worth being precise about, because on a first read it looks like the
opposite.

The plane has no local file. SQLite runs over a VFS whose pages live in FoundationDB, so a
commit is a FoundationDB transaction and it costs about 1080 microseconds. That sounds like
durability back on the write path.

It is not, because the unit changed. A write inside a transaction touches the page cache
and costs nothing. Only `COMMIT` crosses the network. So the millisecond is paid once for
each transaction rather than once for each write, and an actor that batches its work pays
it once for all of it.

What that does put on the path is the page miss. A read of a page that is not cached is a
round trip, at the same 1080 microseconds. That is why read-ahead is the engineering in a
VFS over a network, and `../logbook/store_plane.md` records the measurements.

The honest cost is that a crash can lose the last few unreplicated writes. We take
that trade deliberately, and it is worth stating plainly rather than burying: weft
prefers to lose a few milliseconds of the newest state over making every write 14
times slower. For a world of moving avatars that is obviously right. For a bank it
would be obviously wrong. Knowing which kind of system you are building is the whole
decision.

FoundationDB still holds ownership and receives the replica, so an actor can move to
another machine and find its state there.

## Tail latency is the number that matters

Average latency is a comfortable number that hides the problem. What a person feels is
the worst frame, not the mean one.

Most of the choices above are really about tails:

- Reliable control rides separate WebTransport streams, so one slow transfer does not
  block another behind it.
- Live updates ride datagrams with a sequence number, so a late packet is dropped
  rather than queued. Queueing a late packet delays every packet after it.
- No game packet enters the BEAM, so no packet triggers garbage collection.
- The sandbox costs process setup once, at start, and never on a call.

The pattern is the same each time: refuse to let slow work sit in front of fast work.

## What this page does not claim

These are the measured properties of parts, on one developer machine. weft has never
run with real players, and the whole system under real load is not measured. See
"What we have not proven" in `how-it-works.md`.

One number puts the rest in proportion. Everything above is measured in microseconds,
and the network trip to a person's headset is measured in tens of milliseconds. The
internal budget is under half a percent of the total. That is not a reason to stop
caring about it. It is a reason to know why we care: not to win the total, but so that
nothing internal ever becomes the thing that stutters.
