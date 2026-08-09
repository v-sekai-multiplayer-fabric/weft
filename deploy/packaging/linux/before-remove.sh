#!/bin/sh
# Stop and disable the weft services before the package is removed.
set -e
systemctl disable --now weft.service || true
systemctl disable --now versitygw.service || true
