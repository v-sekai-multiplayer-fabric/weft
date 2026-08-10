# Godot client (desktop and VR)

Goal: a Godot client for avatar play and for observing. It runs in three modes: HMD,
desktop, and TUI.

State: not started. The Godot Linux binary is available in the godot-images repository. The
latest tag is v2026.06.27.1907-multiplayer-fabric. The editor binary
`godot.linuxbsd.editor.double.x86_64` runs headless (Godot 4.7 beta). So this task is
buildable now.

Next: scaffold a Godot project (`project.godot`, a fabric client script, and a headless
`tui_observer.gd`). Run it headless with the godot-images binary. The client connects to a
WebTransport zone server. Run the TUI mode on GitHub Actions for QA.
