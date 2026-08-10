# Run weft in containers

One image runs weft on both platforms. Docker on Windows uses Compose. Fedora Linux uses
Podman Quadlets under systemd. The image versions match the CI: Elixir 1.20, OTP 29, and
FoundationDB 7.3.76.

The store plane needs FoundationDB. Each flow starts a single-node FoundationDB, shares
the cluster file through a volume, and points `WEFT_FDB_CLUSTER_FILE` at it.

## The release stages

RFD 0067 sets three stages: dev, beta, and rc. `../.github/workflows/release-native.yml`
builds them.

**Built.** The workflow builds the OTP release and the C++ data plane on Linux, and it
packages the cluster as a `.deb` and a `.rpm`. The packages install the services enabled,
and they vendor the FoundationDB installers, so a node is self-contained. Burrito and zig
were dropped.

**Not built.** Validation of the packaging in CI, and the FoundationDB install in
particular.

Linux is the only release target. FoundationDB publishes no Windows build after 7.2.5, and
the pinned 7.3.76 release has no Windows asset, so a Windows node cannot run the store
plane. `erlfdb` has no Windows arm in its build script either. Windows runs weft in a
container, which the rest of this page describes.

## Files

- `Containerfile` builds and runs weft, and nothing else. It carries no iceoryx2 and no
  Rust toolchain: a plane reaches the data plane over iceoryx2, and the BEAM reaches the
  data plane through the NIF, so the BEAM never speaks iceoryx2. Each plane is its own
  image. See `../native/README.md`.
- `compose.yaml` runs weft and FoundationDB with Docker Compose.
- `quadlet/` holds the Podman Quadlet units for systemd.
- `packaging/` holds the OS package inputs. `release-native.yml` builds the .deb and the
  .rpm from them.

## Docker on Windows

You need Docker Desktop with the WSL 2 backend.

```
docker compose -f deploy/compose.yaml up --build
```

Compose starts FoundationDB, configures the database one time, then starts weft. To run
the tests or the stress bench instead, override the command:

```
docker compose -f deploy/compose.yaml run --build weft mix test
```

## systemd Quadlets on Fedora Linux

You need Podman 5 or later. The units run rootless under your user session.

1. Build the image.

   ```
   podman build -t weft:dev -f deploy/Containerfile .
   ```

2. Install the units.

   ```
   mkdir -p ~/.config/containers/systemd
   cp deploy/quadlet/* ~/.config/containers/systemd/
   systemctl --user daemon-reload
   ```

3. Start the services.

   ```
   systemctl --user start weft-fdb.service
   systemctl --user start weft.service
   ```

4. Configure the FoundationDB database one time.

   ```
   podman exec weft-fdb fdbcli --exec 'configure new single memory'
   ```

For a system service instead of a user service, copy the units to
`/etc/containers/systemd/` and use `systemctl` without `--user`.

## Notes

- The `weft` service publishes port 4000 for the client WebTransport and HTTP/3 endpoint.
- The `fdb` volume holds the FoundationDB data and the cluster file. Remove it to reset the
  database.
- `ELIXIR_IMAGE` and `FDB_VERSION` are build arguments on the `Containerfile`. Override
  them to change the versions.
