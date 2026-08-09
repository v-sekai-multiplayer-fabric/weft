# weft

weft is the Elixir and OTP control plane of the multiplayer fabric.

It runs one shared 3D world for 1000 people or more, at low latency. A usual game server
runs one main loop. weft cuts that loop into parts, and each part runs on its own. The
parts together are the mesh.

New here? Read **[docs/how-it-works.md](docs/how-it-works.md)** first. It explains the
whole system in simple words, and it needs no knowledge of Elixir or game engines. Then
read [docs/planes.md](docs/planes.md) for the full architecture.
