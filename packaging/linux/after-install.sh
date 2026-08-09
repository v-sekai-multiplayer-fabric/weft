#!/bin/sh
# After the package installs, enable a one-shot bootstrap. The bootstrap runs outside
# the package-manager lock, so it can install the vendored FoundationDB. This script does
# not use the network.
set -e
mkdir -p /var/lib/weft/s3
systemctl daemon-reload || true
systemctl enable --now weft-bootstrap.service || true
