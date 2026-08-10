# CLAUDE.md

Rules and conventions for the weft repo. Follow them.

## What weft is

- weft is the Elixir and OTP control plane of the multiplayer fabric.
- The docs in `docs/` are the source of truth. Start with `docs/reference/architecture.md`.
- A page that describes unbuilt work says so at the top, in a "What is built" note. A
  reader must not need the task pages to tell design from running code.
- `docs/essays/how-it-works.md` is the plain explanation for a new reader. Keep it correct.

## Writing

Two kinds of prose, two rules. Pick by what the text does, not by where it lives.

**Normative and reference text** states a rule, a term, or a contract. A reader looks it
up and must not misread it. Write it in ASD-STE100 Simplified Technical English:

- Use short sentences. The maximum is 20 words.
- Do not use contractions. Do not use semicolons. Use a simple tense.
- This covers everything in `docs/reference/`, plus `CLAUDE.md` and every moduledoc and
  comment.

**Explanatory text** makes a reader understand why. Write it as an essay, not as a
manual. STE forbids the things that make an explanation readable, so do not use it here:

- Start with a question, not with the answer. If the page opens by stating its
  conclusion and then defends it, it is a memo, not an essay.
- Follow the surprise. If nothing in a section contradicts what a reader would assume,
  cut the section.
- Vary sentence length. Flat rhythm hides which sentence carries the weight.
- Name the cost of a decision, not only the benefit.
- This covers everything in `docs/essays/`.

Both kinds: use one name for one concept. Terms are in `docs/reference/architecture.md` and
`CITATION.cff` (Khronos CATSG). Do not name another company or product.

## Architecture

- The BEAM runs only the control plane. Every heavy plane is a native process outside the BEAM.
- Planes talk over Eclipse iceoryx, zero-copy. Never a Port.
- A plane has no networking. An edge is a plane with networking. This is a definition,
  not a default, so there is no exception to check.
- An edge obeys every plane rule and adds one capability, the network. It terminates a
  transport and gives the decoded result to a plane over iceoryx. An edge holds no
  authority, runs no simulation, and keeps no durable state.
- A plane runs a thin C++ thread-per-core harness over iceoryx v1, not Seastar and not Rust. One runtime model for all planes. iceoryx v1 needs the RouDi daemon.
- Durable state is FoundationDB, over the network with `erlfdb`. iceoryx does not cross machines.
- The store is a native plane. It tiers a local SQLite WAL primary to a FoundationDB replica to S3-compatible object storage.
- The S3-compatible endpoint is `versitygw`. FoundationDB backs up to it with `fdbbackup`.
- The asset CDN uses casync through the `desync` fork. Chunks go into SQLite to FoundationDB to the S3-compatible tier, not a naive S3 CDN.
- The native data plane is C++ at `native/dataplane`. It is a seqlock ring built with CMake.
- The client transport is HTTP/3 and WebTransport. Never HTTP/1.1.
- Authority is the single writer of an entity. Interest is a read-only `CH_INTEREST` replica.

## Code

- Do not use exceptions for control flow, in Elixir or in native code.
- Return `{:ok, _}` and `{:error, _}` tuples.
- Low latency is the priority. Keep durability and replication off the write path.
- Match every enum variant. Do not use a catch-all arm.
- Do not use Rust. The target environment blocklists it. Native planes are C++.
- Formalize an algorithm in Lean4 first, then port it to Elixir. The spec lives in
  `docs/spec/`. Read `docs/spec/README.md` first.
- Lean4 proofs use `native_decide`. Do not use Mathlib. Elixir tests mirror the proofs.
- The store copies rivet's Depot layout: PIDX, DELTA by txid, and SHARD by `as_of_txid`.
  `docs/spec/Store.lean` is the spec. Compaction adds a shard version. It never
  overwrites one, and it clears a PIDX row only when that row points at a folded txid.
- Do not add a tuning constant. A constant is a guess about a workload we have not seen.
  Derive a limit from a physical limit, such as the FoundationDB value size. Trigger work
  on a ratio between two measured sizes, such as log bytes against base bytes. A ratio
  has no units to tune, and it moves with the load on its own.
- Planes use Eclipse iceoryx v1, never iceoryx2. v1 needs the RouDi daemon beside each
  plane, so the version changes what a machine runs.

## Git and CI

- main requires a pull request and the merge queue (RFD 0021). No direct or force push to main.
- Commit titles are sentence case. Do not use a conventional-commit prefix. Do not mention an agent.
- Do not commit binaries or recordings to git. Upload them as CI artifacts.
- CI is GitHub Actions. `ci.yml` runs the tests on each pull request.
- `stress-bench.yml` records the 3D scope and gathers the `docs/essays/benchmarks.md` numbers.

## Release

- The release is a standard OTP release. It bundles ERTS, so it is self-contained. Do not use Burrito or zig.
- The cluster packages as a `.deb` and a `.rpm`. Linux is the only release target.
- Do not add a Windows release target. FoundationDB has no Windows build after 7.2.5, so a
  Windows node cannot run the store plane. Run weft on Windows in a container.
- The packages install the services enabled. They bundle FoundationDB, so a node is self-contained.
- `release-native.yml` builds the packages. RFD 0067 sets the dev, beta, and rc stages.

## Tests

- Tests run against real infrastructure, not mocks.
- FoundationDB tests are tagged `:fdb`. They skip when no cluster is present.

## Layout

Keep the top level small. Put a new file in one of these directories.

- `lib/` and `test/` hold the Elixir control plane.
- `native/` holds every native source. `native/dataplane` is the C++ plane. `native/nif` is
  the NIF.
- `test/bench/` holds every benchmark. `test/bench/sumo` is the SUMO trace. `test/bench/fly` is the Fly
  network test.
- `deploy/` holds every ship and run artifact. `deploy/packaging` builds the OS packages.
  `deploy/quadlet` runs the Podman units.
- `docs/` holds the prose. `docs/spec` holds the Lean4 specs.

## Docs

The directory says which kind of page it holds. Put a new page in the right one.

- `docs/reference/` holds the rules, the terms, and the contracts. Start at
  `docs/reference/architecture.md`, which is the architecture of the whole system.
  `docs/reference/` holds one page for each open task, with its goal, its state today,
  and its next step.
- `docs/essays/` explains why those rules exist. Send a newcomer to
  `docs/essays/how-it-works.md`.
- `docs/spec/` holds the Lean4 specs. `docs/spec/README.md` explains how a test mirrors
  a proof.
