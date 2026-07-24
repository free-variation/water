#!/bin/sh
# Golden-output harness for the loadable-library tests (tests/lib/*.h2o).
#
# These tests `load` a lib/ library and need its external dependencies —
# LAPACK through the vendored liblapacke_water shared library, and (for the
# xgboost tests) libxgboost. The core `make test` suite excludes them so it
# builds and passes without those deps installed. Native-only: the wasm build
# excludes the FFI, so there is no wasm counterpart.
#
# Same <name>.h2o / <name>.expected pairing as run.sh. Run from the repo root
# (make test-libs does), so the tests' relative "lib/…"/"data/…" paths resolve.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/water"

(cd "$root" && make all) || { echo "build failed"; exit 1; }

pass=0
fail=0

for input in "$here"/lib/*.h2o; do
    [ -e "$input" ] || { echo "no lib tests found"; exit 1; }
    name=$(basename "$input" .h2o)
    expected="$here/lib/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected file)"
        continue
    fi
    actual=$(mktemp "${TMPDIR:-/tmp}/water.XXXXXX")
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
echo "$pass passed, $fail failed (lib)"

[ "$fail" -eq 0 ]
