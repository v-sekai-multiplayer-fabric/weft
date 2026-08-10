# weft

weft is the Elixir and OTP control plane of the multiplayer fabric.

It runs one shared 3D world at low latency. A usual game server runs one main loop. weft
cuts that loop into parts, and each part runs on its own. The parts together are the
mesh.

weft is early work. The parts are measured. The whole system is not. See "What we have
not proven" in the page below.

New here? Read **[how it works](docs/essays/how-it-works.md)** first. It explains the whole
system in simple words, and needs no knowledge of Elixir or game engines.

The docs split by what they do:

- **A rule lives with the code it governs.** [`lib/weft.ex`](lib/weft.ex) is the
  architecture of the whole system. An Elixir rule is a moduledoc, and a native rule is a
  `README.md` beside the source. Each also carries its own open task, so a task page
  cannot go stale away from its code.
- **[docs/essays/](docs/essays/)** explains why those rules exist.
- **[docs/logbook/](docs/logbook/)** holds every measurement, with the machine and the
  settings that produced it.
- **[docs/spec/](docs/spec/)** holds the Lean4 specs that the Elixir tests mirror.
