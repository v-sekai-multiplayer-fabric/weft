# Unfinished tasks

This file records the open work so we can pause and resume. Each entry has the goal, the
state today, and the next step. The date is 2026-08-10.

The rules that apply to every task are in `CLAUDE.md`.

## Done recently

- #52 colored, dithered braille scope. `docs/spec/Dither.lean` holds the Floyd-Steinberg spec
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

Goal: a dev-stage release of the whole system, per RFD 0067. Elixir builds a standard OTP
release. Godot packages from the godot-images templates. The stages are dev, beta, and rc.

State: mostly built. `release-native.yml` builds the OTP release and the C++ data plane on
Linux. It packages the cluster as a .deb and a .rpm. The packages install the services
enabled. The package vendors the FoundationDB installers. Burrito and zig were dropped.

The Windows release target was dropped. FoundationDB publishes no Windows build after
7.2.5, and the pinned 7.3.76 release has no Windows asset, so a Windows node cannot run
the store plane. `erlfdb` also has no Windows arm in its build script. Windows runs weft
in a container. See `deploy/README.md`.

Next: export the Godot package with the godot-images templates and add it to the packages.
Validate the packaging in CI, especially the FoundationDB install.

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
