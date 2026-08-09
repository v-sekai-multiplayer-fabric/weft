# weft

weft is the Elixir and OTP control plane of the multiplayer fabric.

The docs are the single source of truth. Start with `docs/planes.md`. Terms are
defined there and in `CITATION.cff` (Khronos CATSG). This README stays thin on purpose,
so it does not desync from the code and the docs.

## Docs

- `docs/planes.md` — the planes rule, the terms, the clients.
- `docs/yagni.md` — the build-now plan.
- `docs/data-plane.md` — the game data plane boundary and the ring.
- `docs/store.md` — the store plane.
- `docs/protocol.md` — the wire formats.
- `docs/latency.md` — the low-latency rules.
- `docs/runtime-choice.md` — the stage tier and the asset CDN.
- `docs/benchmarks.md` — the performance numbers.

## Quality gates

```sh
mix test
mix dialyzer
```
