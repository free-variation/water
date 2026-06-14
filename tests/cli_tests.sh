#!/bin/sh
# Command-line flag tests for logicforth.
#
# The golden-output harness (run.sh) always invokes the binary with `-b`, so it
# can't exercise flag behavior itself. These cases run the binary directly with
# various flags and check stdout+stderr and exit status. Grow by adding `exact`
# / `has` lines below.
#
# Run standalone with: sh tests/cli_tests.sh   (run.sh also calls it.)

set -u
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/../logicforth"
pass=0
fail=0

ok()  { pass=$((pass + 1)); printf "  ok   %s\n" "$1"; }
bad() { fail=$((fail + 1)); name=$1; shift; printf "  FAIL %s\n" "$name"; for l in "$@"; do printf "       %s\n" "$l"; done; }

# run INPUT FLAGS... -> sets $out (stdout+stderr, trailing newline stripped) and $code
run() {
    input=$1
    shift
    out=$(printf '%s\n' "$input" | "$bin" "$@" 2>&1)
    code=$?
}

# exact NAME INPUT EXPECTED_OUT EXPECTED_CODE FLAGS...
exact() {
    name=$1 input=$2 want=$3 wantcode=$4
    shift 4
    run "$input" "$@"
    if [ "$out" = "$want" ] && [ "$code" = "$wantcode" ]; then ok "$name"
    else bad "$name" "flags: $*" "want (exit $wantcode): [$want]" "got  (exit $code): [$out]"; fi
}

# has NAME INPUT SUBSTRING EXPECTED_CODE FLAGS...
has() {
    name=$1 input=$2 sub=$3 wantcode=$4
    shift 4
    run "$input" "$@"
    case "$out" in *"$sub"*) found=1 ;; *) found=0 ;; esac
    if [ "$found" = 1 ] && [ "$code" = "$wantcode" ]; then ok "$name"
    else bad "$name" "flags: $*" "want substring (exit $wantcode): [$sub]" "got (exit $code): [$out]"; fi
}

printf "CLI flag tests:\n"

# batch mode: only the program's own output, no banner, no prompt
exact "batch (-b) is quiet"             '2 3 + . cr'  "5 " 0 -b
exact "piped default is batch"          '2 3 + . cr'  "5 " 0
# interactive: banner + per-line prompt (banner version not pinned)
has   "interactive (-i) shows banner"   '2 3 + . cr'  "logicforth " 0 -i
has   "interactive (-i) shows prompt"   '1 . cr'      "0 ok"          0 -i
# --max-objects lowers the object ceiling so the limit is reachable cheaply
has   "--max-objects hits ceiling"      '1 200000 range [: drop < 0 > :] map drop'  "object registry full" 0 -b --max-objects 100000
# --max-objects argument validation
has   "--max-objects needs a value"     ''  "needs a value"      2 --max-objects
has   "--max-objects rejects 0"         ''  "positive integer"   2 --max-objects 0
has   "--max-objects rejects non-number" '' "positive integer"   2 --max-objects xyz
# unknown flag is rejected
has   "unknown flag rejected"           ''  "unknown option"     2 --bogus

# a truncated image must fail cleanly (no crash) and leave the interpreter
# usable: load-image errors, then the next line still computes 2 3 + = 5
img=$(mktemp "${TMPDIR:-/tmp}/lf_img.XXXXXX")
printf ': sq dup * ; variable v < 1 2 3 > to v [ 10 20 30 ] "%s" save-image\n' "$img" | "$bin" -b >/dev/null 2>&1
imgsize=$(wc -c < "$img")
trunc=$(mktemp "${TMPDIR:-/tmp}/lf_trunc.XXXXXX")
head -c $((imgsize / 2)) "$img" > "$trunc"
out=$(printf '"%s" load-image\n< 9 8 7 > gc 2 3 + . cr\n' "$trunc" | "$bin" -b 2>&1)
code=$?
case "$out" in
    *error:*5*) ok "truncated image: clean error + recovery" ;;
    *) bad "truncated image: clean error + recovery" "want an error then 5 (exit 0)" "got (exit $code): [$out]" ;;
esac
rm -f "$img" "$trunc"

printf "%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
