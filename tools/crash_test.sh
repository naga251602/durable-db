#!/usr/bin/env bash
# Crash test: kill the writer at many different moments, reopen the database,
# and check that no acknowledged write was lost and nothing else appeared.
#
#   Mode 0 (sweep):        crash at EVERY crash point of a short run, one by one.
#   Mode 1 (random point): crash at a random crash point in a longer run.
#   Mode 2 (random time):  an outside `kill -9` at a random time.
#
# "Crash points" are places inside the engine (see include/durabledb/fault.h)
# where it SIGKILLs itself: inside WAL append, and between every step of
# flush and compaction.
#
# Note: kill -9 tests PROCESS crashes. The OS page cache survives, so this does
# not model power loss (that would need a VM or dm-log-writes style tool).
#
# usage: tools/crash_test.sh [build_dir] [rounds_for_modes_1_and_2]
set -u
BUILD=${1:-build}
ROUNDS=${2:-30}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0

# $1 = label. Uses $WORK/acks.txt and $WORK/db. Prints nothing on success if QUIET=1.
check() {
  local last
  last=$(grep ' \.$' "$WORK/acks.txt" | tail -n 1 | awk '{print $2}')
  if [ -z "$last" ]; then [ "${QUIET:-0}" = 1 ] || echo "  $1: nothing acknowledged yet, skipped"; return; fi
  # verify twice: the second open checks that recovery itself is repeatable
  if ! "$BUILD/crash_verify" "$WORK/db" "$last" > "$WORK/verify.txt" 2>&1 ||
     ! "$BUILD/crash_verify" "$WORK/db" "$last" > /dev/null 2>&1; then
    echo "  $1: FAILED (last ack $last)"; cat "$WORK/verify.txt"; fail=1
  elif [ "${QUIET:-0}" != 1 ]; then
    echo "  $1: $(cat "$WORK/verify.txt")"
  fi
}

echo "== Mode 0: sweep every crash point of a 300-op run =="
# memtable 512 bytes: a flush every ~25 ops, a compaction every 3 flushes
n=0
for target in $(seq 1 5000); do
  rm -rf "$WORK/db"
  ( DURABLEDB_CRASH_AT=$target "$BUILD/crash_writer" "$WORK/db" 512 300 0 > "$WORK/acks.txt" 2>/dev/null || true ) 2>/dev/null
  if grep -q 'ACK 299 \.' "$WORK/acks.txt"; then break; fi   # ran to the end: no more crash points
  QUIET=1 check "crash point #$target"
  n=$target
done
echo "  swept $n crash points"

echo "== Mode 1: crash at a random crash point (longer run, fsync on) =="
for r in $(seq 1 "$ROUNDS"); do
  rm -rf "$WORK/db"
  target=$(( (RANDOM % 4000) + 1 ))
  ( DURABLEDB_CRASH_AT=$target "$BUILD/crash_writer" "$WORK/db" 1024 2500 1 > "$WORK/acks.txt" 2>/dev/null || true ) 2>/dev/null
  check "round $r (crash point #$target)"
done

echo "== Mode 2: kill -9 at a random time =="
for r in $(seq 1 "$ROUNDS"); do
  rm -rf "$WORK/db"
  { "$BUILD/crash_writer" "$WORK/db" 1024 > "$WORK/acks.txt" 2>/dev/null & } 2>/dev/null
  pid=$!
  sleep "0.$(( (RANDOM % 8) + 1 ))"
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  check "round $r (random kill)"
done

if [ $fail -eq 0 ]; then echo "ALL CRASH TESTS PASSED"; else echo "CRASH TESTS FAILED"; exit 1; fi
