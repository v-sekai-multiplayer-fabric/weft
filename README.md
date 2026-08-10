# weft

weft is the Elixir and OTP control plane of the multiplayer fabric.

It runs one shared 3D world at low latency. A usual game server runs one main loop. weft
cuts that loop into parts, and each part runs on its own. The parts together are the
mesh.

weft is early work. The parts are measured. The whole system is not. See "What we have
not proven" in the page below.

New here? Read **[how it works](docs/essays/how-it-works.md)** first. It explains the whole
system in simple words, and needs no knowledge of Elixir or game engines.
