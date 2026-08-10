# native/edge

The two edges. An edge is a plane with networking.

`Weft` defines both. This directory holds the code, and that moduledoc holds the reason.

| directory | edge | terminates | gives the result to |
| --- | --- | --- | --- |
| `ingest` | Ingest | player input datagrams | the game data plane |
| `gateway` | Gateway | client control streams | the control plane |
| `transport` | shared | QUIC and WebTransport | both of the above |

State: `transport` holds working picoquic code. `ingest` and `gateway` hold a contract and
no code. Neither can build until iceoryx v1 and RouDi are in the container image.

## What an edge may do, and what it may not

An edge follows the plane contract without change. It is a separate native process. It is
sandboxed, crash isolated, and thread per core, and the control plane reaches it over
iceoryx.

The one difference is that the sandbox keeps the network.

- It terminates a transport and decodes the wire format.
- It holds no authority. It decides nothing about the game.
- It runs no simulation.
- It keeps no durable state.

So a network attacker reaches a process that can decide nothing. That is what the split
buys, and it is the reason an edge is a separate process instead of a thread.

## Why two edges and not one

A client holds one session to each edge. That costs two handshakes and two congestion
controllers on one link.

weft accepts that cost. The datagram path and the control path run in separate processes,
so control work cannot delay the datagrams. A login, a chat message, or an asset pull is
reliable and low rate. Player input is unreliable and high rate. One process for both puts
the slow work in front of the fast work.

## The shared transport

`transport` came from `zone-server-h2o`, where it ran in the process that holds authority.
Read `TRANSPORT.md` for what moved and what it costs.

Both edges terminate the same transport, so the code is here once. The client transport is
HTTP/3 and WebTransport, and never HTTP/1.1. Firefox speaks both.

## What is not here

The Gyre web client. It is a client, not an edge, and it lives in `zone-guest-gyre`. Its
`slughorn.wasm` and its vendored bundle were the two binaries in this repository that broke
the rule against committing a binary to git. Removing the client removed them.

A deployment. `deploy/` holds every ship and run artifact, and `native/` holds source.
`native/dataplane` is the pattern: a `CMakeLists.txt` and a `src`, and nothing else.
