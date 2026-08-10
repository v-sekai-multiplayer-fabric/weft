# What carries a plane on another machine?

weft has a hole, and it is in the middle rather than at an edge.

Two planes on one machine talk over iceoryx2 at 214 M snapshots each second. Two planes on
different machines talk through the store plane, at 12918 commits each second, which is
1161 times short of the tick. `../logbook/data_plane.md` records the row with nothing in
it: per-tick state across machines, no design and no number.

The question is what fills that row. rivet already answered a version of it, so the honest
first move is to read what they built rather than invent.

## What rivet built

rivet runs actors on runners, and a runner is a process somewhere else. That is weft's
problem with different words: a plane is a process somewhere else, and the control plane
has to reach it.

Five things carry it, and none of them is the transport.

**One versioned wire schema.** `engine/sdks/schemas/runner-protocol/` holds `v1.bare`
through `v7.bare`. A connection names its version at init, and `conn.rs` keeps
`protocol_version: u16` for the life of the connection. Seven versions coexist in one
tree. That is the shape of a protocol that shipped and then had to change without a
flag day.

**One connection carries everything.** `ToServer` is a union of init, events, command
acknowledgements, stopping, pong, key-value requests, and a tunnel message. There is no
second socket for control and no third for data.

**The tunnel is a union inside that union.** `ToServerTunnelMessageKind` carries an HTTP
response start, a response chunk, a response abort, and four WebSocket frames. So an
actor's own traffic rides the runner's connection, and the actor never learns what carried
it.

**Liveness is a task, not a field.** `ping_task.rs` is its own file, and it updates the
ping of every runner at once rather than per connection. Liveness is not a property the
data path reports as a side effect.

**Size is the transport's problem.** `universalpubsub/src/chunking.rs` splits a message
into a start and chunks with a message id, and reassembles behind a buffer with a maximum
age of 300 s. Above that sits a driver: memory, NATS, or Postgres. The application never
sees a size limit, and the driver can be swapped.

## The one that answers the store plane too

Looking for something else found this, which is the part worth the most.

`pegboard-envoy` has `actor_sqlite_page_task.rs` and `actor_remote_sqlite_task.rs`, and
`envoy-protocol` carries what they need:

    type SqliteGetPagesRequest struct {
        actorId: Id
        pgnos: list<SqlitePgno>
        expectedGeneration: optional<u64>
        expectedHeadTxid: optional<u64>
    }
    type SqliteFetchedPage struct { pgno: SqlitePgno, bytes: optional<SqlitePageBytes> }
    type SqliteDirtyPage   struct { pgno: SqlitePgno, bytes: SqlitePageBytes }

**The pages travel over the connection.** A runner asks for page numbers and gets page
bytes back. It does not hold a FoundationDB client, and it does not know FoundationDB
exists.

Read what the request carries and the design falls out of it. `expectedGeneration` and
`expectedHeadTxid` are a fence, checked on every read rather than at commit time, which is
the bug `../../native/storeplane/README.md` records weft finding in its own VFS.
`SqliteFetchedPage.bytes` is optional, so a page that was never written comes back absent
rather than as zeroes.

For weft this is one substitution and not a rewrite. `fdb_vfs.c` already gives SQLite its
pages one at a time. Today `xRead` reaches FoundationDB with `libfdb_c`. Point it at the
link instead and a plane on another machine runs SQLite with no client, no cluster file,
and no credentials.

It also settles the rule in `Weft.Limits` about who holds the FoundationDB client. The
answer stops being "the store plane, and every other plane asks it" and becomes "the
control plane, and a plane asks for pages". One fewer process holds a database client.

The cost is a round trip for a page miss. `../logbook/store_plane.md` measures that floor
at 1080 us, and it is why read-ahead is the engineering in a VFS over a network.

## What weft should take, and what it should not

Take the five, and take the page protocol. Do not take the transport.

One thing is not there to take. rivet's GUARD is not Envoy. `engine/` has no match for
`envoyproxy`, `xds`, or `XDS`. `guard-core` is their own Rust on hyper, with a certificate
resolver, a proxy service, and a custom serve trait. `pegboard-envoy` is their own package
and the word means an emissary, not the proxy.

rivet's H3 and WebTransport code, in `guard-core/src/h3_server.rs` and
`datagram_transport.rs`, was hacked in rather than designed in. It is not the part worth
copying, and reading it as a model would copy a shortcut.

Take the transport decision instead, which weft already made for a different reason. The
client transport is HTTP/3 and WebTransport, never HTTP/1.1. picoquic is already vendored
in `fabric-edge`. So a plane on another machine reaches the gateway edge the same way a
browser does.

That is the saving. One transport to get right rather than two.

## Why not the iceoryx2 tunnel

iceoryx2 v0.9.3 ships one, in `iceoryx2-services/tunnel`, over zenoh. Run
`iox2 tunnel zenoh` on each host and publish-subscribe spans machines. It works today,
which nothing weft would write does.

Three costs, and each one undoes a decision weft already made.

It is a daemon, one for each host. weft left the first generation of iceoryx partly because
it needed a daemon beside every plane, and this is that shape returning under another
name.

It is not in the C ABI. `iceoryx2.h` has no `tunnel` symbol, so the `.sigs` dispatch table
cannot reach it. It is a Rust binary to run, not a library to call, which puts it outside
the arrangement that keeps Rust out of weft's build graph.

It is a second QUIC implementation. weft already carries picoquic for the edge, and zenoh
brings its own.

None of that makes it wrong. It makes it a different bet: a working tunnel now against one
transport later. Measuring it between two machines would decide, and nobody has.

## The part that is still not solved

None of this makes a cross-machine tick fast. rivet's shape is a control plane reaching a
runner, at the rate a control plane works. A tick has 66 ns for each snapshot, and a
network hop is four orders of magnitude away from that.

So the honest position is that this link carries commands, lifecycle, and state that can
be late. One world still runs in one machine. `topology.md` says that already, and nothing
here changes it.

What the link buys is that a plane can be somewhere else at all.
