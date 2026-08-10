#!/usr/bin/env sh
# Soak the FoundationDB VFS. Run it inside the container, beside a live FoundationDB.
#
#   soak.sh <seconds>
#
# Each round loads the database, then asks SQLite to check itself with
# `PRAGMA integrity_check`. That check is SQLite's own, not ours, so it catches a page
# the VFS corrupted even where our expectations would not.
#
# It prints a line for each round, so a slowdown or a leak shows as a trend rather than
# as one number at the end.
set -eu

SECONDS_TO_RUN=${1:-3600}
ROWS=${ROWS:-2000}
export WEFT_FDB_CLUSTER_FILE=${WEFT_FDB_CLUSTER_FILE:-/var/fdb/fdb.cluster}
export WEFT_EXCLUSIVE=1 WEFT_CACHE=1

start=$(date +%s)
round=0
fail=0

printf '%-6s %-9s %-14s %-14s %-12s %s\n' round elapsed point_reads inserts rss_kb integrity

while :; do
	now=$(date +%s)
	elapsed=$((now - start))
	[ "$elapsed" -ge "$SECONDS_TO_RUN" ] && break
	round=$((round + 1))

	out=$(/tmp/bench "$ROWS" 2>&1) || { echo "round $round: bench failed"; fail=$((fail + 1)); continue; }
	reads=$(echo "$out" | awk '/point read/ {print $3}')
	inserts=$(echo "$out" | awk '/one commit each/ {print $5}')

	# SQLite checks its own pages. This is the reusable suite, not one we wrote.
	check=$(/tmp/integrity bench_fdb.db 2>&1 | tail -1)
	[ "$check" = "ok" ] || fail=$((fail + 1))

	rss=$(awk '/VmHWM/ {print $2}' /proc/self/status 2>/dev/null || echo 0)
	printf '%-6s %-9s %-14s %-14s %-12s %s\n' "$round" "$elapsed" "$reads" "$inserts" "$rss" "$check"
done

echo
echo "rounds: $round, failures: $fail, seconds: $((`date +%s` - start))"
[ "$fail" -eq 0 ] || exit 1
