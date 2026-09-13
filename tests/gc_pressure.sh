#!/bin/sh
# GC-rooting-under-pressure tests for telic.
#
# Each case runs a word that allocates a fresh object while an operand (or an
# in-flight C-level value) is the only reference to some heap value, in a loop
# under a small --max-objects ceiling. Each loop allocates thousands of
# transient objects, so the table fills and forces a mark-sweep collection
# *inside* the allocating helper many times over; if the operand is not rooted
# across that allocation the collector frees it and the word reads freed
# memory, so the accumulated result comes out wrong (or the process crashes
# under ASan). A correct, deterministic total proves the operand stayed live.
#
# These need the --max-objects flag, so the golden harness (run.sh, which only
# ever runs `-b` with the program on stdin) cannot express them; they live here
# and run.sh invokes this file. The ceiling (500) is a fixed generous headroom
# above the embedded library's object count — large enough that growing the
# library never trips it, small enough that the loops' allocations still overflow
# it and force collection.
#
# Run standalone with: sh tests/gc_pressure.sh   (run.sh also calls it.)

set -u
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/../telic"
pass=0
fail=0

ok()  { pass=$((pass + 1)); printf "  ok   %s\n" "$1"; }
bad() { fail=$((fail + 1)); name=$1; shift; printf "  FAIL %s\n" "$name"; for l in "$@"; do printf "       %s\n" "$l"; done; }

# exact NAME PROGRAM EXPECTED_OUT MAX_OBJECTS
exact() {
    name=$1 program=$2 want=$3 max=$4
    out=$(printf '%s\n' "$program" | "$bin" -b --max-objects "$max" 2>&1)
    code=$?
    if [ "$out" = "$want" ] && [ "$code" = 0 ]; then ok "$name"
    else bad "$name" "want (exit 0): [$want]" "got  (exit $code): [$out]"; fi
}

printf "GC-rooting-under-pressure tests:\n"

# binary_op / unary_op operands: the float fast path exits early, but the
# heavy tag path allocates a result matrix while the operand matrix is live
# (verified to fail without the roots: binary case yielded 99144).
exact "binary_op operands stay rooted" \
  ': pressure 0 to acc 0 to i begin i 500 < while [ 1 2 3 4 5 6 7 8 ] vector 2 ^ sum acc + to acc f++ i repeat acc ; pressure . cr' \
  "102000 " 500
exact "mod operands stay rooted" \
  ': pressure 0 to acc 0 to i begin i 500 < while [ 5 7 9 11 ] vector 3 mod sum acc + to acc f++ i repeat acc ; pressure . cr' \
  "2500 " 500
exact "unary_op operand stays rooted" \
  ': pressure 0 to acc 0 to i begin i 500 < while [ 1 2 3 ] vector exp sum acc + to acc f++ i repeat acc round ; pressure . cr' \
  "15096 " 500
exact "unify rest-pattern operands stay rooted" \
  ': pressure 0 to acc 0 to i lvar to tail begin i 500 < while [ 1 2 3 ] [ _ tail rest ] ~ drop tail ? 1 @i acc + to acc f++ i repeat acc ; pressure . cr' \
  "1500 " 500

# matrix row/column/where words allocate their result while the source matrix
# is the only reference; the source must stay rooted across that allocation.
exact "@j source matrix stays rooted" \
  ': pressure 0 to acc 0 to i begin i 500 < while [ 1 2 3 4 ] 2 2 matrix 0 @j sum acc + to acc f++ i repeat acc ; pressure . cr' \
  "2000 " 500
exact "@i row matrix stays rooted" \
  ': pressure 0 to acc 0 to i begin i 500 < while [ 1 2 3 4 ] 2 2 matrix 0 @i sum acc + to acc f++ i repeat acc ; pressure . cr' \
  "1500 " 500
exact "where source mask stays rooted" \
  ': pressure 0 to acc 0 to i begin i 500 < while [ 0 1 0 1 1 ] vector where sum acc + to acc f++ i repeat acc ; pressure . cr' \
  "4000 " 500

printf "%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
