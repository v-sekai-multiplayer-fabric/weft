# Run weft in containers

One image runs weft on both platforms. Docker on Windows uses Compose. Fedora Linux uses
Podman Quadlets under systemd. The image versions match the CI: Elixir 1.20, OTP 29, and
FoundationDB 7.3.76.

The store plane needs FoundationDB. Each flow starts a single-node FoundationDB, shares
the cluster file through a volume, and points `WEFT_FDB_CLUSTER_FILE` at it.

## Files

- `Containerfile` builds and runs weft. It also builds iceoryx2, the zero-copy bus
  between planes, in a stage of its own so the Rust toolchain does not reach the runtime
  image. iceoryx2 is brokerless, so a node runs no daemon for it.
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
