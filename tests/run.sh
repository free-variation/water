#!/bin/sh
# Golden-output test harness for logicforth.
#
# Each test is a pair of files in this directory:
#   <name>.l4        — input piped to the REPL on stdin
#   <name>.expected  — exact stdout the REPL should produce
#
# Tests run in alphabetical order. The exit code is 0 if every test
# passes, 1 otherwise — suitable for CI.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/logicforth"

(cd "$root" && make logicforth) || { echo "build failed"; exit 1; }

pass=0
fail=0

for input in "$here"/*.l4; do
    [ -e "$input" ] || { echo "no tests found"; exit 1; }
    name=$(basename "$input" .l4)
    expected="$here/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected file)"
        continue
    fi
    actual=$(mktemp "${TMPDIR:-/tmp}/logicforth.XXXXXX")
    # Drop the startup banner (line 1) so the version isn't baked into every
    # expected file; expected files start from the first command's output.
    "$bin" < "$input" 2>&1 | tail -n +2 > "$actual"
    if diff -q "$expected" "$actual" > /dev/null 2>&1; then
        pass=$((pass + 1))
        printf "  ok   %s\n" "$name"
    else
        fail=$((fail + 1))
        printf "  FAIL %s\n" "$name"
        diff -u "$expected" "$actual" | sed 's/^/       /'
    fi
    rm -f "$actual"
done

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
