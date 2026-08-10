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
| `edge` | The ingest edge, the gateway edge, and the transport they share. | weft, over `zone-server-h2o`'s transport |

## Plane or edge

A plane has no networking. An edge is a plane with networking. That is a definition and not
a default, so there is no exception to check.

`Weft.PlaneNetworkingTest` enforces it. It reads the source of each plane and fails if a
plane calls a socket function, includes a transport header, vendors one, or links one. It
does not read `edge`, which may do all of this.

`Weft` names the two edges and says what each terminates. `edge/README.md` holds the same
table beside the code.

## The fork

`gyreplane` started as a subtree of `zone-server-h2o`. It is a fork now.

    git subtree add --prefix=native/gyreplane \
      https://github.com/v-sekai-multiplayer-fabric/zone-server-h2o.git main --squash

`zone-server-h2o` terminated QUIC in the process that holds authority, so the transport and
its libraries moved to `edge`. The plane then lost the h2o request half, the libriscv guest
sandbox, and every vendored dependency, because none of them had a caller. It is 280 kB,
down from 22 MB.

The cost is real. `git subtree pull` conflicts on every file that moved or went, and there
are many. Read `edge/TRANSPORT.md` and `../docs/reference/gyre_plane.md` for the list.

`README.md` inside `gyreplane` belongs to its upstream, except for a note at the top that
records the change. `edge/README.md` is weft's own.

## The subtree that contributed nothing

`zone-guest-gyre` came in as a second subtree, at `native/gyreedge`. Not one file from it
survives.

It held a browser client, which is not an edge, and a Fly deployment that could not build,
because every path its Containerfile compiled had already been deleted upstream. Everything
in `edge` today is either weft's own or `zone-server-h2o`'s transport.

The client is not lost. `zone-guest-gyre` maintains it, which is where a client belongs.
