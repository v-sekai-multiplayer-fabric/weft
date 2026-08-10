# Actor limits

Goal: enforce the limits at the store, the gateway, and the lifecycle.

State: built. `Weft.Limits` holds the values, and it is enforced.

| limit | value | where it is enforced |
| --- | --- | --- |
| storage for one actor | 10 GiB | `Weft.Actor`, before the write |
| one key | 2 KiB | `Weft.Actor`, before the write |
| one value | 128 KiB | `Weft.Actor`, before the write |
| one action | 60 s | `Weft.Limits.with_in_flight/1` |
| requests for each minute for each address | 1200 | `Weft.Gateway.dispatch/1` |
| requests in flight | 32 | `Weft.Gateway.dispatch/1` |

## What enforces each one

weft holds the numbers. It does not hold the mechanism, because each mechanism exists
already.

- **The rate for each address** is Hammer, counted across the cluster. Each node counts
  what it sees and tells the others, which is the method in the guide of Hammer for a
  distributed ETS backend. weft sends over `:pg`, which is in OTP.
- **The requests in flight** is `Task.Supervisor` with `max_children`. OTP counts the
  children and refuses the one that goes over. A child that dies is removed by its
  supervisor, so a crash cannot leak a slot.
- **The time for one action** is a receive timeout.
- **The sizes** are `byte_size/1` on the encoded term.

## A limit refuses, and it does not drop

`Weft.Actor.put/3` returns `{:error, {:limit, which, limit: _, actual: _}}` and writes
nothing. `Weft.Gateway.dispatch/1` returns the same shape. The error names the limit and
the size, so a caller reports the cause and does not guess.

A request with no address is a call from inside the node. It is not counted, because
these limits bound what one caller outside may take.

## Where 32 comes from

The other values are promises to the person who writes an actor. This one is measured.

`store_plane_logbook.md` sweeps the number of commits in flight from 1 to 512. The
latency is nearly flat to about 64 and then carries the load. So 32 sits inside the flat
part, at 1.7 times the unloaded latency.

## What the rate limit does not do

The count is eventually consistent. A burst that lands on two nodes at once can pass the
limit, a node that joins starts with an empty window, and a network split lets each side
allow the whole limit.

Each of these fails open, which is the correct direction. A limit that failed closed
would drop good traffic on a node that had just started.

## Next

Enforce the action limit on the actor call itself, and not only on a gateway request. An
actor action that runs long inside a `handle_call` still holds its process.
