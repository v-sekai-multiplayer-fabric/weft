# native

Every native source of weft. The BEAM runs the control plane only. Every heavy plane is a
native process outside the BEAM.

This directory holds source. `../deploy/` holds every ship and run artifact.
`native/dataplane` is the shape to copy: a `CMakeLists.txt` and a `src`, and nothing else.

## What each directory is

| directory | what it is | origin |
| --- | --- | --- |
| `dataplane` | The seqlock ring. C++, built with CMake. | weft |
| `nif` | The NIF that the control plane loads. | weft |
| `storeplane` | The SQLite VFS over FoundationDB. | weft |
| `gyreplane` | The zone server. An FDB zone tick, and nothing else. | fork of `zone-server-h2o` |
| `edge` | The ingest edge, the gateway edge, and the transport they share. | weft, over a fork of `zone-guest-gyre` |

## Plane or edge

A plane has no networking. An edge is a plane with networking. That is a definition and not
a default, so there is no exception to check.

`Weft.PlaneNetworkingTest` enforces it. It reads the source of each plane and fails if a
plane calls a socket function, includes a transport header, vendors one, or links one. It
does not read `edge`, which may do all of this.

`Weft` names the two edges and says what each terminates. `edge/README.md` holds the same
table beside the code.

## The two forks

`gyreplane` and `edge` both started as a subtree. Neither one is a subtree now.

`zone-server-h2o` terminated QUIC in the process that holds authority, so the transport and
its libraries moved to `edge`. The plane then lost the h2o request half, the libriscv guest
sandbox, and every vendored dependency, because none of them had a caller. It is 280 kB,
down from 22 MB.

The cost is real. `git subtree pull` conflicts on every file that moved or went, and there
are many. Read `edge/TRANSPORT.md` and `../docs/reference/gyre_plane.md` for the list.

`README.md` inside `gyreplane` belongs to its upstream, except for a note at the top that
records the change. `edge/README.md` is weft's own, because the edge is weft's design and
not a copy of anything.

The command that added each one, before the fork:

    git subtree add --prefix=native/gyreplane \
      https://github.com/v-sekai-multiplayer-fabric/zone-server-h2o.git main --squash

    git subtree add --prefix=native/gyreedge \
      https://github.com/v-sekai-multiplayer-fabric/zone-guest-gyre.git main --squash

The second prefix is `native/edge` now, and the browser client that came with it is gone. A
client is not an edge. It lives in `zone-guest-gyre`, where it is maintained.
