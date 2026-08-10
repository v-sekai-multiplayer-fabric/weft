# native

Every native source of weft. The BEAM runs the control plane only. Every heavy plane is a
native process outside the BEAM.

## What each directory is

| directory | what it is | origin |
| --- | --- | --- |
| `dataplane` | The seqlock ring. C++, built with CMake. | weft |
| `nif` | The NIF that the control plane loads. | weft |
| `storeplane` | The SQLite VFS over FoundationDB. | weft |
| `gyreplane` | The zone server. An FDB zone tick, and nothing else. | fork of `zone-server-h2o` |
| `gyreedge` | The browser client, and the transport that serves it. | fork of `zone-guest-gyre` |

## The two forks

Each of the two came in as a subtree. Neither one is a subtree any more.

A plane has no networking. `zone-server-h2o` terminated QUIC in the process that holds
authority, so the transport and its libraries moved to `gyreedge`. That edit is inside the
subtree, which makes it a fork.

The plane lost more than the transport. It lost the h2o request half, the libriscv guest
sandbox, and every vendored dependency, because none of them had a caller. It is 328 kB,
down from 22 MB.

The cost is real. `git subtree pull` conflicts on every file that moved or went, and there
are many. Read `gyreedge/TRANSPORT.md` and `../docs/reference/gyre_plane.md` for the list.

`README.md` inside each fork belongs to its upstream, except for a note at the top of
`gyreplane/README.md` that records the change. Read `../docs/reference/gyre_plane.md` for
what weft does with them.

The command that added each one, before the fork:

    git subtree add --prefix=native/gyreplane \
      https://github.com/v-sekai-multiplayer-fabric/zone-server-h2o.git main --squash

    git subtree add --prefix=native/gyreedge \
      https://github.com/v-sekai-multiplayer-fabric/zone-guest-gyre.git main --squash

## Which side a directory sits on

A plane has no networking. An edge is a plane with networking.

`gyreplane` holds authority, so it is plane work. `gyreedge` holds the client, so the edge
exists to serve it.

`zone-server-h2o` does both at once today. It terminates QUIC in the process that holds
authority. Splitting that is open work, and `../docs/reference/gyre_plane.md` states the
order.
