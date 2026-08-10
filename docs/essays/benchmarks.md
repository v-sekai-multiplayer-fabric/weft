# What the benchmarks changed our minds about

The numbers live in the logbooks, with the machine and the settings that produced them:
`../logbook/control_plane.md`, `../logbook/data_plane.md`, and
`../logbook/store_plane.md`. This page is what we learned, which is a different
thing and does not go stale when a machine changes.

Three results moved a decision. Two of them moved it away from where we expected.

## The store we measured was not the store we ship

The first store bench reported 70 µs for a write, and that number sat on this page for
months. It was measuring `Store.Sqlite`, which fsyncs on every commit. weft ships
`Store.Replicated`, which uses WAL without an fsync per commit, and nobody had ever
pointed a bench at it.

The real write costs 19 µs. The reported figure was wrong by 288 times against the
slowest row in the same table.

What makes this worth a page rather than a correction note is the second finding inside
it. On that host a local write with fsync costs 5.5 ms, and a synchronous write to
FoundationDB over the network costs 1.9 ms — *the local disk was slower than the
network database*. Everyone's intuition says local beats remote. Here the pragma beat the
backend, by a factor larger than the distance to the database.

And that multiple does not travel: on the Linux runner the same write costs 70 µs, not
5.5 ms. The ordering survives the move between hosts. The magnitude does not. A benchmark
that reports a ratio without its host is reporting a property of the host.

## Compute was never the bottleneck, so we stopped optimizing it

The target is 15 million packets per second. The obvious question is whether decode and
apply can keep up.

One core does 826 million. That is 55 times the target, and it means the target needs
about two percent of one core.

So the interesting number is not the fast one. Feed the same code an entity table too
large for cache and a core drops to 41 million — twenty times slower — and the aggregate
across sixteen cores flattens at 117 million, which is the memory bandwidth of the
machine rather than anything about the code.

Both numbers say the same thing, and it is not the thing a compute benchmark usually
says. Even the pessimistic figure clears the target by 2.7 times, so no amount of faster
decoding buys anything. What caps 15 million packets per second is moving them from the
card into memory. That is why the ladder is AF_XDP, then DPDK, then a SmartNIC, and why
the hot path stays native and out of the BEAM. It is not because decoding is expensive.
It is because decoding is free and the I/O is not.

## The message-passing ceiling is real, and the fix is not a faster message

Handing snapshots to the BEAM one Erlang message at a time tops out near 1.4 million per
second, because each message copies a term into a mailbox. Nothing about that improves
with a bigger machine.

A shared slot that the writer overwrites and the BEAM samples does 2.85 million on one
core, and because each zone owns a ring it reaches 27.7 million at sixteen. Sampling
costs about 3 µs, so reading at 60 Hz costs nothing worth counting.

The lesson generalises past this benchmark: the copy was the cost, so the answer was to
stop copying, not to make the copy faster.

## A measurement that failed, and why it stays written down

An attempt to measure how fast the kernel receives packets gave 0.16 million per second
on loopback, and `recvmmsg` was no faster than `recv`.

That second half is the useful part. A batching syscall that does not beat a per-packet
syscall is not measuring what it claims to measure. The batch had nothing to batch,
because the bottleneck was the send side of loopback, not the receive path. The number
was not slow. It was meaningless, and the absence of a gain is what proved it.

Loopback is not a proxy for a network card. The real number waits for hardware, and the
logbook says so rather than quietly dropping the run.

## Why the numbers are not the proof

Every result above measures a mechanism. None of them answers the question the product
asks, which is whether somebody can be present in a shared world without motion sickness,
at scale.

A transport benchmark proves a transport is fast. It cannot prove presence. The SUMO
world in VR is the testbed for that, and `yagni.md` explains why we treat it as the proof
rather than any table here.
