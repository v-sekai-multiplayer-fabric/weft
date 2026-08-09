#!/bin/sh
# Install the vendored FoundationDB client and server, then enable the weft services.
# This runs from the weft-bootstrap one-shot, outside the package-manager lock. No
# network. The FoundationDB server package configures and starts a single node.
set -e
V=/opt/weft/vendor

if ! command -v fdbserver >/dev/null 2>&1; then
  if command -v dpkg >/dev/null 2>&1; then
    dpkg -i "$V"/foundationdb-clients_*.deb "$V"/foundationdb-server_*.deb || true
  elif command -v rpm >/dev/null 2>&1; then
    rpm -i "$V"/foundationdb-clients-*.rpm "$V"/foundationdb-server-*.rpm || true
  fi
fi

systemctl enable --now weft.service || true
systemctl enable --now versitygw.service || true

# Run once.
systemctl disable weft-bootstrap.service || true
