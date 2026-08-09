# CLAUDE.md

Rules and conventions for the weft repo. Follow them.

## What weft is

- weft is the Elixir and OTP control plane of the multiplayer fabric.
- The docs in `docs/` are the source of truth. Start with `docs/planes.md`.

## Writing

- Write all prose in ASD-STE100 Simplified Technical English.
- Use short sentences. The maximum is 20 words.
- Do not use contractions. Do not use semicolons. Use a simple tense.
- Use one name for one concept. Terms are in `docs/planes.md` and `CITATION.cff` (Khronos CATSG).

## Architecture

- The BEAM runs only the control plane. Every heavy plane is a native process outside the BEAM.
- Planes talk over Eclipse iceoryx2, zero-copy. Never a Port. Networking is off by default.
- A plane always uses Seastar (thread-per-core). There is one runtime model for all planes.
- Durable state is FoundationDB, over the network with `erlfdb`. iceoryx2 does not cross machines.
- The store is a native plane. It is a local SQLite WAL primary with an async FoundationDB replica.
- The asset CDN uses casync through the `desync` fork. Chunks go into SQLite to FoundationDB, not S3.
- The client transport is HTTP/3 and WebTransport. Never HTTP/1.1.
- Authority is the single writer of an entity. Interest is a read-only `CH_INTEREST` replica.

## Code

- Do not use exceptions for control flow, in Elixir or in native code.
- Return `{:ok, _}` and `{:error, _}` tuples.
- Low latency is the priority. Keep durability and replication off the write path.
- Match every enum variant. Do not use a catch-all arm.
- Formalize an algorithm in Lean4 first, then port it to Elixir. The spec lives in `lean/`.
- Lean4 proofs use `native_decide`. Do not use Mathlib. Elixir tests mirror the proofs.

## Git and CI

- main requires a pull request and the merge queue (RFD 0021). No direct or force push to main.
- Commit titles are sentence case. Do not use a conventional-commit prefix. Do not mention an agent.
- Do not commit binaries or recordings to git. Upload them as CI artifacts.
- CI is GitHub Actions. `ci.yml` runs the tests on each pull request.
- `stress-bench.yml` records the 3D scope and gathers the `docs/benchmarks.md` numbers.

## Tests

- Tests run against real infrastructure, not mocks.
- FoundationDB tests are tagged `:fdb`. They skip when no cluster is present.

## Reference docs

- `docs/planes.md`, `docs/data-plane.md`, `docs/store.md`, `docs/protocol.md`, `docs/latency.md`.
- `docs/yagni.md`, `docs/runtime-choice.md`, `docs/benchmarks.md`.
- `docs/tasks.md` records the open work, the state today, and the next step.
