#!/bin/sh
# Enable and start the weft services after the package installs.
set -e
mkdir -p /var/lib/weft/s3
systemctl daemon-reload || true
systemctl enable --now weft.service || true
systemctl enable --now versitygw.service || true
