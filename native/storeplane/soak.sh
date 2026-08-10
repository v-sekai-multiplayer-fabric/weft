#!/usr/bin/env sh
# Soak the FoundationDB VFS. Run it inside the container, beside a live FoundationDB.
#
#   soak.sh <seconds>
#
# Run it with `docker compose run -T`. Without `-T` compose wants a terminal, and in a
# script it produces no output at all, so a soak runs blind.
#
# A normal round loads the database, then asks SQLite to check itself with
# `PRAGMA integrity_check`. That check is SQLite's own, not ours, so it catches a page
# the VFS corrupted even where our expectations would not.
#
# Every third round is adversarial: a writer is killed with SIGKILL in the middle of a
# commit, and the database is checked again. The kill delay moves each time, because the
# commit window is short and a fixed delay rarely lands inside it.
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

printf '%-6s %-9s %-10s %-14s %-14s %s\n' round elapsed kind point_reads inserts integrity

while :; do
	now=$(date +%s)
	elapsed=$((now - start))
	[ "$elapsed" -ge "$SECONDS_TO_RUN" ] && break
	round=$((round + 1))

	if [ $((round % 3)) -eq 0 ]; then
		# Adversarial. The delay walks across the commit window instead of sitting in
		# one place, so a torn commit has a chance to show.
		delay=$(((round * 37) % 900 + 50))
		out=$(/tmp/crash soak_crash.db "$ROWS" "$delay" 2>&1) || fail=$((fail + 1))
		check=$(echo "$out" | awk '/integrity_check/ {print $2}')
		if echo "$out" | grep -q TORN; then
			fail=$((fail + 1))
			check=TORN
		fi
		printf '%-6s %-9s %-10s %-14s %-14s %s\n' "$round" "$elapsed" "kill+$delay" - - "$check"
	else
		out=$(/tmp/bench "$ROWS" 2>&1) || {
			echo "round $round: bench failed"
			fail=$((fail + 1))
			continue
		}
		reads=$(echo "$out" | awk '/point read/ {print $3}')
		inserts=$(echo "$out" | awk '/one commit each/ {print $5}')

		# SQLite checks its own pages. This is the reusable suite, not one we wrote.
		check=$(/tmp/integrity bench_fdb.db 2>&1 | tail -1)
		[ "$check" = "ok" ] || fail=$((fail + 1))

		printf '%-6s %-9s %-10s %-14s %-14s %s\n' "$round" "$elapsed" load "$reads" "$inserts" "$check"
	fi
done

echo
echo "rounds: $round, failures: $fail, seconds: $((`date +%s` - start))"
[ "$fail" -eq 0 ] || exit 1
