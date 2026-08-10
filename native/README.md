# native

Every native source of weft. The BEAM runs the control plane only. Every heavy plane is a
native process outside the BEAM.

## What each directory is

| directory | what it is | origin |
| --- | --- | --- |
| `dataplane` | The seqlock ring. C++, built with CMake. | weft |
| `nif` | The NIF that the control plane loads. | weft |
| `storeplane` | The SQLite VFS over FoundationDB. | weft |
| `gyreplane` | The zone server. Transport, an FDB zone tick, and a guest sandbox. | subtree of `zone-server-h2o` |
| `gyreedge` | The browser client that the edge serves. | subtree of `zone-guest-gyre` |

## The two subtrees

A subtree holds upstream code. Do not edit a file inside one to fix weft. Send the change
upstream, and pull the subtree again.

`README.md` inside each subtree belongs to its upstream. It describes that repository and
not this one. Read `../docs/reference/gyre_plane.md` for what weft does with them.

The command that added each one:

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
