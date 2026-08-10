#!/bin/sh
# Runs as the fdb-local container's PID 1. Writes a cluster file on
# first boot only (so a restart -- the crash-recovery test task #21
# needs -- reconnects to the same database instead of creating a new
# empty one), starts fdbserver in the background, waits for it to
# accept connections, then runs `configure new single memory` once
# (idempotent: skipped if the database already exists, checked via
# `status minimal` rather than blindly re-running configure and
# hoping the "already configured" error is harmless).
#
# fdbserver runs backgrounded (not exec'd directly) so this script can
# still run the one-time configure step after it starts, but a trap
# forwards SIGTERM to it so `podman stop` / `systemctl stop` still
# stops the real server process promptly and cleanly, not just this
# shell.
set -e

DATA_DIR=/var/fdb/data
LOG_DIR=/var/fdb/logs
CLUSTER_FILE=/var/fdb/fdb.cluster

mkdir -p "$DATA_DIR" "$LOG_DIR"

if [ ! -f "$CLUSTER_FILE" ]; then
  DESC=$(tr -dc 'a-zA-Z0-9' </dev/urandom | head -c8)
  ID=$(tr -dc 'a-zA-Z0-9' </dev/urandom | head -c8)
  echo "${DESC}:${ID}@127.0.0.1:4500" > "$CLUSTER_FILE"
  echo "fdb-local: wrote new cluster file: $(cat "$CLUSTER_FILE")" >&2
else
  echo "fdb-local: reusing existing cluster file: $(cat "$CLUSTER_FILE")" >&2
fi

fdbserver \
  --cluster-file "$CLUSTER_FILE" \
  --datadir "$DATA_DIR" \
  --logdir "$LOG_DIR" \
  --listen-address 127.0.0.1:4500 \
  --public-address 127.0.0.1:4500 &
FDB_PID=$!
trap 'echo "fdb-local: forwarding SIGTERM to fdbserver ($FDB_PID)" >&2; kill -TERM "$FDB_PID"; wait "$FDB_PID"' TERM INT

for i in $(seq 1 30); do
  if fdbcli -C "$CLUSTER_FILE" --exec "status minimal" 2>/dev/null | grep -q "The database is available"; then
    echo "fdb-local: database already configured" >&2
    break
  fi
  if fdbcli -C "$CLUSTER_FILE" --exec "configure new single memory" 2>/dev/null; then
    echo "fdb-local: configured new single-process database" >&2
    break
  fi
  sleep 1
done

wait "$FDB_PID"
