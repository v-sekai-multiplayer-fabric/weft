# dev-stage release

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
