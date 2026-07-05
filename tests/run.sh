#!/bin/sh
# Golden-output test harness for water.
#
# Each test is a pair of files in this directory:
#   <name>.h2o        — input piped to the REPL on stdin
#   <name>.expected  — exact stdout the REPL should produce
#
# Tests run in alphabetical order. The exit code is 0 if every test
# passes, 1 otherwise — suitable for CI.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/water"

(cd "$root" && make all) || { echo "build failed"; exit 1; }

pass=0
fail=0

for input in "$here"/*.h2o; do
    [ -e "$input" ] || { echo "no tests found"; exit 1; }
    name=$(basename "$input" .h2o)
    expected="$here/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected file)"
        continue
    fi
    actual=$(mktemp "${TMPDIR:-/tmp}/water.XXXXXX")
    # Batch mode (-b): no banner, no per-line prompt — just the program's own
    # output (and errors), so expected files hold exactly what the script prints.
    "$bin" -b < "$input" > "$actual" 2>&1
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

echo
sh "$here/cli_tests.sh"
cli_status=$?

[ "$fail" -eq 0 ] && [ "$cli_status" -eq 0 ]
