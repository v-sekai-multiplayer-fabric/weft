# CLAUDE.md

Rules and conventions for the weft repo. Follow them.

## What weft is

- weft is the Elixir and OTP control plane of the multiplayer fabric.
- The docs in `docs/` are the source of truth. Start with `Weft`.
- A page that describes unbuilt work says so at the top, in a "What is built" note. A
  reader must not need the task pages to tell design from running code.
- `docs/essays/how-it-works.md` is the plain explanation for a new reader. Keep it correct.

## Writing

Two kinds of prose, two rules. Pick by what the text does, not by where it lives.

**Normative and reference text** states a rule, a term, or a contract. A reader looks it
up and must not misread it. Write it in ASD-STE100 Simplified Technical English:

- Use short sentences. The maximum is 20 words.
- Do not use contractions. Do not use semicolons. Use a simple tense.
- This covers `CLAUDE.md`, `docs/logbook/`, every moduledoc, every README beside code, and
  every comment.

**Explanatory text** makes a reader understand why. Write it as an essay, not as a
manual. STE forbids the things that make an explanation readable, so do not use it here:

- Start with a question, not with the answer. If the page opens by stating its
  conclusion and then defends it, it is a memo, not an essay.
- Follow the surprise. If nothing in a section contradicts what a reader would assume,
  cut the section.
- Vary sentence length. Flat rhythm hides which sentence carries the weight.
- Name the cost of a decision, not only the benefit.
- This covers everything in `docs/essays/`.

Both kinds: use one name for one concept. Terms are in `Weft` and
`CITATION.cff` (Khronos CATSG). Do not name another company or product.

## Architecture

- The BEAM runs only the control plane. Every heavy plane is a native process outside the
  BEAM, in a repository of its own and a container of its own. weft does not start it and
  does not restart it. The platform does, and today that is a Fly app for each plane.
  `native/` holds the data plane and the NIF, and nothing else.
- Do not build a path that carries per-tick state between machines. One core at the DRAM
  bound covers about 1493 SUMO-scale worlds, so it answers a workload nothing here has
  measured. `docs/essays/yagni.md` holds the arithmetic and names what would change the
  answer, which is a measured workload that does not fit in one machine.
  `Weft.VocabularyTest` blocks the names it would arrive under.
- Two planes on one machine talk over iceoryx2, zero copy. Two planes on different
  machines do not talk directly at all. They go through the store plane to FoundationDB,
  which is a global transaction and is slow. The 10 GiB limit for one actor is sized for
  it.
- HTTP/3 and WebTransport is the client transport, terminated at an edge. It is not an
  internal path between planes.
- So a plane may be its own Fly app, and what it can reach follows from where it lands.
- A plane reaches the data plane over iceoryx2. The BEAM reaches the data plane through the
  NIF. So the BEAM never links iceoryx2, and a plane is a black box to it except for what
  that plane writes to the ring.
- Planes talk over Eclipse iceoryx, zero-copy. Never a Port.
- A plane has no networking. An edge is a plane with networking. This is a definition,
  not a default, so there is no exception to check.
- An edge obeys every plane rule and adds one capability, the network. It terminates a
  transport and gives the decoded result to a plane over iceoryx. An edge holds no
  authority, runs no simulation, and keeps no durable state.
- A plane runs a thin C++ thread-per-core harness over iceoryx2, not Seastar. One runtime
  model for all planes. iceoryx2 is brokerless, so a machine runs no daemon beside a plane.
- Durable state is FoundationDB, over the network with `erlfdb`. iceoryx does not cross machines.
- The store is a native plane. SQLite runs inside it with a VFS whose pages live in
  FoundationDB. There is no local database file, so an actor's database moves between
  machines with no copy and no restore. `PRAGMA journal_mode=MEMORY` keeps SQLite from
  writing one. rivet lists the same rule as binding, for the same reason: a local file
  makes storage stateful and not migratable.
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
- Do not write Rust. The target environment blocklists it. Native planes are C++.
  iceoryx2 is the one exception, and it is a dependency and not weft code. weft builds it
  and links its C++ bindings. See `docs/essays/runtime-choice.md`.
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
- Planes use Eclipse iceoryx2, never iceoryx v1. v1 does not build here, because it needs
  libacl. iceoryx2 needs no daemon, so a machine runs one less process.

## Git and CI

- main requires a pull request and the merge queue (RFD 0021). No direct or force push to main.
- Commit titles are sentence case. Do not use a conventional-commit prefix. Do not mention an agent.
- Do not commit binaries or recordings to git. Upload them as CI artifacts.
- CI is GitHub Actions. `ci.yml` runs the tests on each pull request.
- `stress-bench.yml` records the 3D scope and gathers the numbers. A measured number goes
  in a logbook under `docs/logbook/`, with the machine and the settings that produced it.
  An essay says what a measurement changed, and it does not hold the table.

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
- `native/` holds the data plane and the NIF, and nothing else. `native/dataplane` is the
  seqlock ring, and `native/nif` is the NIF the BEAM loads. A new plane is a new
  repository, and it takes `fabric-harness` as a subtree if it needs the bus. See
  `native/README.md`.
- `test/bench/` holds every benchmark. `test/bench/sumo` is the SUMO trace. `test/bench/fly` is the Fly
  network test.
- `deploy/` holds every ship and run artifact. `deploy/packaging` builds the OS packages.
  `deploy/quadlet` runs the Podman units.
- `docs/` holds the prose. `docs/spec` holds the Lean4 specs, and `docs/logbook` holds the
  measurements.

## Docs

The directory says which kind of page it holds. Put a new page in the right one.

- **A rule, a term, or a contract lives with the code it governs.** Start at `Weft`, which
  is the architecture of the whole system. An Elixir rule is a moduledoc. A native rule is
  a `README.md` beside the source, or `WEFT.md` where the `README.md` belongs to an
  upstream fork.
- **An open task lives on the same page**, with its goal, its state today, and its next
  step. There is no separate task directory, because a task page away from its code goes
  stale while still reading as authoritative.
- `docs/essays/` explains why those rules exist. Send a newcomer to
  `docs/essays/how-it-works.md`.
- `docs/logbook/` holds every measurement, oldest entry first.
- `docs/spec/` holds the Lean4 specs. `docs/spec/README.md` explains how a test mirrors
  a proof.
