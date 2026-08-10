# Local CI, via podman + systemd quadlet

`Containerfile` in this directory is a step-by-step mirror of
`.github/workflows/real-build.yml` -- same dependency versions, same
build flags, same per-test compile recipes, in the same order. A pass
here means the same thing a green `real-build.yml` run means.

It exists for one concrete reason. GitHub Actions itself was in a
platform-wide major outage on 2026-08-06. Githubstatus.com reported
delayed and failed workflow runs, plus queued jobs that time out.
Task #18 could not wait on that outage. Task #18 needed to reverify
the real build after the mbedtls-to-OpenSSL switch.

## Usage

```sh
cp ci-local/zone-server-h2o-ci.build.example \
   ~/.config/containers/systemd/zone-server-h2o-ci.build
sed -i "s|REPO_PATH|$(pwd)|g" \
   ~/.config/containers/systemd/zone-server-h2o-ci.build
systemctl --user daemon-reload
systemctl --user start zone-server-h2o-ci-build.service
journalctl --user -u zone-server-h2o-ci-build.service -f
```

`systemctl --user status zone-server-h2o-ci-build.service` after it
finishes reports success or failure the same way a CI job's own
pass/fail does. This is a `Type=oneshot` unit: it builds the image
once and exits, it does not run as a persistent daemon.

```sh
podman run --rm localhost/zone-server-h2o-ci:latest true
```

confirms the built image (and therefore the full build and every
`test/unit/*.c` it ran during `podman build`) is real and intact.

## Local deployment (task #20)

The same image doubles as a runtime deployment target -- see the
`ENTRYPOINT` note at the bottom of `Containerfile`. Two more quadlets
stand up a full local deployment: a real single-process FoundationDB
(`zone-server-h2o-fdb.build`/`.container`) and `zone-server-h2o` itself
(`zone-server-h2o.container`). Both use `Type=notify`, the normal shape
for a long-running service. `Type=oneshot` cannot work for a process
that keeps listening. Both stay on-demand: neither has an `[Install]`
section, so neither starts at boot or auto-restarts.

```sh
# Real cert/key first (see scripts/generate-tls-cert.sh's own header
# for the cert_hash this prints, needed for Uro's POST /shards later).
./scripts/generate-tls-cert.sh

podman volume create zone-server-h2o-fdb-data

for f in zone-server-h2o-fdb.build zone-server-h2o-fdb.container zone-server-h2o.container; do
  cp "ci-local/$f.example" "$HOME/.config/containers/systemd/$f"
  sed -i "s|REPO_PATH|$(pwd)|g; s|CERTS_PATH|$(pwd)/certs|g" \
    "$HOME/.config/containers/systemd/$f"
done

systemctl --user daemon-reload
systemctl --user start zone-server-h2o.service   # pulls in the fdb build/container too
journalctl --user -u zone-server-h2o.service -f
```

A successful start logs `webtransport_server: WebTransport bound on
UDP 7443, path /zone, zone 0 (TLS cert/key loaded)`. `ss -uln | grep
7443` confirms the real UDP listener. `ss -tln | grep 4500` confirms
FDB's real TCP listener. FDB's wire protocol is TCP, not UDP.

`podman exec zone-server-h2o-fdb fdbcli -C /var/fdb/fdb.cluster --exec
"status minimal"` should report "The database is available."

Check SELinux mode first with `getenforce`. Under `Enforcing`, the
bind-mounted cert directory needs `:Z`, and the shared FDB volume
needs `:z` (lowercase -- two different containers read it). Both
`.example` files already have the right one on each line. Without it,
cert loading fails with "Cannot load certificate" despite correct file
permissions. This was hit and root-caused, not guessed. See
`zone-server-h2o.container.example`'s own comment.

## Why a few packages are listed here that real-build.yml does not list

GitHub's `ubuntu-latest` runner image ships `make`, `clang`,
`zlib1g-dev`, and `adduser` preinstalled. A plain `ubuntu:24.04`
container image ships none of these. This `Containerfile` installs
them explicitly, confirmed by the real errors the first few local
build attempts hit without each one (`CMAKE_MAKE_PROGRAM is not set`,
`Could NOT find ZLIB`, FoundationDB's postinst needing `adduser`), not
guessed. Every other dependency and step matches `real-build.yml`
verbatim.
