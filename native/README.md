# native

The data plane, and nothing else.

weft is the control plane. The BEAM runs it, and every heavy plane is a native process
outside the BEAM.

| directory | what it is |
| --- | --- |
| `dataplane` | The seqlock ring. A plane writes it, the BEAM samples it. C++, CMake. |
| `nif` | The NIF the BEAM loads to read that ring. `../Makefile` builds it. |

`nif` is not a plane. It is the BEAM's window onto the ring, and it is the only native code
the BEAM loads into itself.

## Where the planes went

Each one is its own repository, its own process, and its own container. weft does not
supervise them. The platform does, and today that is a Fly app. weft reaches them only
through the data plane, and a plane is opaque to the BEAM in every other way.

| plane or edge | repository |
| --- | --- |
| the harness every plane links | [`fabric-harness`](https://github.com/v-sekai-multiplayer-fabric/fabric-harness) |
| the store plane | [`fabric-store-plane`](https://github.com/v-sekai-multiplayer-fabric/fabric-store-plane) |
| the ingest edge and the gateway edge | [`fabric-edge`](https://github.com/v-sekai-multiplayer-fabric/fabric-edge) |
| the zone plane | [`gyreplane`](https://github.com/v-sekai-multiplayer-fabric/gyreplane) |

Each moved with its history, not as a copy. `fabric-harness` comes into a plane
repository as a `git subtree`, so there is still one `iceoryx2.sigs` and one copy of the
limits.

## Why they left

A plane that lives in this repository is a plane weft builds, and weft does not build
planes. It starts them, watches them, and restarts them.

Keeping them here cost more than it looked. `native/` was 124 MB, and 122 MB of that was
one plane's vendored dependencies and build output. It is 40 kB now.

The rule that follows: a new plane is a new repository. If it needs the bus, it takes
`fabric-harness` as a subtree.

## Which transport a plane uses

It follows from where the plane lands, and not from how the repositories are split.

| the two ends are | path | cost |
| --- | --- | --- |
| in one machine | iceoryx2, zero copy | a pointer handoff |
| in different machines | the store plane, to FoundationDB | a global transaction |

iceoryx2 is shared memory: `/dev/shm` for the segments and `/tmp/iceoryx2` for the service
registry. Two machines share neither, so it is the same-machine path and only that.

There is no direct path between two machines. A plane that needs something another machine
holds reads it from the store. That is a global transaction, and it is slow. The 10 GiB
limit for one actor is sized for it.

HTTP/3 and WebTransport is not an internal path. It is the client transport, and an edge
terminates it.
