# Unfinished tasks

This file records the open work so we can pause and resume. Each entry has the goal, the
state today, and the next step. The date is 2026-08-10.

## Rules that apply to every task

- Change only v-sekai-multiplayer-fabric repositories.
- Land every change through a pull request and the merge queue (RFD 0021). Never push to
  main directly.
- Never commit a binary or a recording to git. Upload it as a CI artifact.
- Write all prose in Simplified Technical English.
- Use one name for one concept.
- Do not use exceptions for control flow. Return `{:ok, _}` or `{:error, _}`.
- Latency is the priority. Keep durability and replication off the write path.

## Done recently

- #52 colored, dithered braille scope. `lean/Dither.lean` holds the Floyd-Steinberg spec
  from paperlesspaper/epdoptimize, proven with `native_decide`. `Weft.DataPlane.Dither` is
  the Elixir port. The scope colors each panel by density, dithers to a six-level palette,
  scales uniformly so it does not skew, sizes to the window, and does not flicker.
- Container dev environments. `deploy/Containerfile`, `deploy/compose.yaml`, and
  `deploy/quadlet/` run weft on Docker (Windows) and Podman Quadlets (Fedora).

## #51 rivet-like actor limits

Goal: add a `Weft.Limits` module. Enforce the limits at the store, the gateway, and the
lifecycle.

State: not started. Values captured: 10 GiB storage, 2 KiB key, 128 KiB value, 60 s per
action, 1200 requests per minute per IP, 32 in-flight requests.

Next: add `Weft.Limits` with these values. Enforce them at the store put, the gateway
request path, and the actor lifecycle. This task is pure Elixir and buildable now.

## #45 Godot client (desktop and VR)

Goal: a Godot client for avatar play and for observing. It runs in three modes: HMD,
desktop, and TUI.

State: not started. The Godot Linux binary is available in the godot-images repository. The
latest tag is v2026.06.27.1907-multiplayer-fabric. The editor binary
`godot.linuxbsd.editor.double.x86_64` runs headless (Godot 4.7 beta). So this task is
buildable now.

Next: scaffold a Godot project (`project.godot`, a fabric client script, and a headless
`tui_observer.gd`). Run it headless with the godot-images binary. The client connects to a
WebTransport zone server. Run the TUI mode on GitHub Actions for QA.

## #24 VR acceptance proof

Goal: prove we can serve at least 1000 HMDs with no motion sickness, with presence, and at
scale.

State: not started. It depends on #45 and the running pipeline. The headless client can
test the pipeline. Real VR presence needs a headset.

Next: define the acceptance test. Drive the SUMO playback through the interest feed to many
simulated HMD observers. Measure the latency and the frame budget.

## #49 dev-stage release

Goal: a dev-stage release of the whole system, per RFD 0067. Elixir saves as Burrito
executables. Godot packages from the godot-images templates. fpm builds the RPMs. The
stages are dev, beta, and rc.

State: not started. The Godot package part is buildable with the godot-images templates.
Burrito needs zig. fpm builds the RPM.

Next: add a release workflow. Build the Burrito executable. Export the Godot package with
the templates. Package both as RPMs with fpm. Tag the dev stage.

## #44 asset CDN baker plane

Goal: a baker plane that turns glb into an OpenUSD stage, plus stage-tier distribution.

State: partial. `Weft.Assets.StageTier` is the desync adapter. The chunks go into
SQLite then FoundationDB. An on-demand H3/WebTransport endpoint serves the chunks. The
baker itself is not built.

Next: build the baker (fabric-stage-runtime plus Adobe glTF). This needs a native OpenUSD
toolchain, which we do not have here yet.

## #48 native store plane

Goal: port the store plane to native. It keeps a local SQLite WAL primary and an async
FoundationDB replica. It runs over Eclipse iceoryx.

State: the Elixir prototype works. `Weft.Actor.Store.Replicated` and `.Replicator` pass the
three FoundationDB tests against a live FoundationDB.

Next: port the store plane to a native process over iceoryx. This needs a native C++ and
iceoryx toolchain, which we do not have here yet.

## Buildable now

- #51 (actor limits, pure Elixir)
- #45 (Godot client, godot-images binary works headless)

## Blocked on a native toolchain

- #44 (OpenUSD baker)
- #48 (native store over iceoryx)
- #49 (needs zig for Burrito; the Godot part is buildable)
