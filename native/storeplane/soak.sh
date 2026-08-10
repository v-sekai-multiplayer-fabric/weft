#!/usr/bin/env sh
# Soak the FoundationDB VFS. Run it beside a live FoundationDB.
#
#   soak.sh <seconds>
#
# A round is one of three kinds, and the script alternates them:
#
#   load    load the database, then ask SQLite to check itself.
#   kill    SIGKILL a writer at a wall clock delay, then look for a torn commit.
#   crash   stop a writer before a numbered commit, then look for a torn commit.
#
# The kill delay and the crash point both move each round, so the faults land at
# different places in a commit rather than at one place over and over.
#
# A soak exists to find a fault that a short test does not. So a failure has to say which
# round, which kind, and what the program printed. A count on its own is half the job,
# and it is the cheap half.
#
# Each failure keeps its database under its own name, so it can be read afterwards
# instead of being overwritten by the next round.
set -eu

SECONDS_TO_RUN=${1:-3600}
ROWS=${ROWS:-2000}
CRASH_ROWS=${CRASH_ROWS:-400}

# The programs CMake builds. Point BIN at the build directory.
BIN=${BIN:-/tmp}
BENCH=${BENCH:-$BIN/bench_vfs}
INTEGRITY=${INTEGRITY:-$BIN/integrity}
CRASH=${CRASH:-$BIN/prove_crash}

export WEFT_FDB_CLUSTER_FILE=${WEFT_FDB_CLUSTER_FILE:-/var/fdb/fdb.cluster}
export WEFT_EXCLUSIVE=1 WEFT_CACHE=1

for prog in "$BENCH" "$INTEGRITY" "$CRASH"; do
	[ -x "$prog" ] || { echo "not executable: $prog" >&2; exit 2; }
done

start=$(date +%s)
round=0

# One counter for each kind of failure. They are different faults and must not share a
# number: a crashed program, a failed integrity check, and a torn commit each mean
# something else.
bench_failed=0
integrity_failed=0
torn=0
crash_failed=0

# Every failure, one line each, printed again at the end.
failures=""

note_failure() {
	# round, kind, reason, output
	echo "  FAILED round $1 ($2): $3"
	echo "$4" | sed 's/^/    | /'
	failures="$failures
  round $1 ($2): $3"
}

# Read what one adversarial round did. `prove_crash` exits 0 when the database it left
# is whole, 1 when it is torn or fails its own check, and 2 when the crash point is past
# the end of the commit sequence. Only the middle one is a fault.
record_crash_round() {
	round=$1 elapsed=$2 kind=$3 detail=$4 rc=$5 out=$6

	case "$rc" in
	0)
		printf '%-6s %-8s %-7s %-10s %-4s %s\n' "$round" "$elapsed" "$kind" "$detail" "$rc" ok
		;;
	2)
		# Not a fault. The crash never happened, so nothing was under test.
		printf '%-6s %-8s %-7s %-10s %-4s %s\n' "$round" "$elapsed" "$kind" "$detail" "$rc" unreached
		;;
	*)
		if echo "$out" | grep -q TORN; then
			torn=$((torn + 1))
			result=TORN
			reason=$(echo "$out" | grep TORN | head -1)
		else
			crash_failed=$((crash_failed + 1))
			result=FAILED
			reason="the crash program exited $rc"
		fi
		printf '%-6s %-8s %-7s %-10s %-4s %s\n' "$round" "$elapsed" "$kind" "$detail" "$rc" "$result"
		note_failure "$round" "$kind" "$reason" "$out"
		;;
	esac
}

printf '%-6s %-8s %-7s %-10s %-4s %s\n' round elapsed kind detail rc result

while :; do
	now=$(date +%s)
	elapsed=$((now - start))
	[ "$elapsed" -ge "$SECONDS_TO_RUN" ] && break
	round=$((round + 1))

	case $((round % 3)) in
	1)
		# A load round. The database keeps its name, because a load round that passes
		# leaves nothing to examine.
		set +e
		out=$("$BENCH" "$ROWS" 2>&1)
		rc=$?
		set -e
		if [ "$rc" -ne 0 ]; then
			bench_failed=$((bench_failed + 1))
			printf '%-6s %-8s %-7s %-10s %-4s %s\n' "$round" "$elapsed" load - "$rc" FAILED
			note_failure "$round" load "the load program exited $rc" "$out"
			continue
		fi

		reads=$(echo "$out" | awk '/point read/ {print $3}')

		# SQLite checks its own pages. This is the reusable suite, not one we wrote. It
		# cannot see a torn commit, which is why the rounds below exist.
		set +e
		check=$("$INTEGRITY" bench_fdb.db 2>&1 | tail -1)
		rc=$?
		set -e
		if [ "$rc" -ne 0 ] || [ "$check" != "ok" ]; then
			integrity_failed=$((integrity_failed + 1))
			printf '%-6s %-8s %-7s %-10s %-4s %s\n' "$round" "$elapsed" load "$reads" "$rc" "$check"
			note_failure "$round" load "integrity_check said $check" "$check"
			continue
		fi
		printf '%-6s %-8s %-7s %-10s %-4s %s\n' "$round" "$elapsed" load "$reads" "$rc" ok
		;;
	2)
		# An adversarial round on the clock. The delay walks across the commit window,
		# because a commit is short and a fixed delay rarely lands inside one.
		delay=$(( (round * 137) % 900 + 50 ))
		db=soak_kill_$round.db
		set +e
		out=$("$CRASH" "$db" "$CRASH_ROWS" kill "$delay" 2>&1)
		rc=$?
		set -e
		record_crash_round "$round" "$elapsed" kill "$delay" "$rc" "$out"
		;;
	*)
		# An adversarial round on a crash point. This one repeats exactly, so a failure
		# here can be reproduced with the same number.
		point=$(( (round * 7) % 90 + 1 ))
		db=soak_crash_$round.db
		set +e
		out=$("$CRASH" "$db" "$CRASH_ROWS" at "$point" 2>&1)
		rc=$?
		set -e
		record_crash_round "$round" "$elapsed" crash "$point" "$rc" "$out"
		;;
	esac
done

echo
echo "rounds: $round, seconds: $(($(date +%s) - start))"
echo "failures: load $bench_failed, integrity $integrity_failed, torn $torn, crash $crash_failed"

if [ -n "$failures" ]; then
	echo
	echo "what failed:$failures"
	exit 1
fi
