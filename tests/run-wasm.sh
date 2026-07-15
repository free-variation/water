#!/bin/sh
# Golden-output test harness for the WASM/WASI build of water.
#
# Mirrors run.sh but runs water.wasm under a WASI runtime instead of
# the native binary. Uses the same <name>.h2o / <name>.expected pairs.
#
# A test may be skipped ONLY when it exercises a feature WASI lacks
# (database, ffi, interactive REPL) — list it in wasm-skip.txt as
# "<name>  <reason>", and the skip is printed with its reason. A test that
# fails for any other reason is a real wasm defect and must not be listed.
#
# Environment:
#   WASM_EXEC command that runs a module (the module path is appended, program
#             on stdin), overriding the default. On a-shell, which has no
#             separate runtime, set WASM_EXEC=wasm.
#   WASMTIME  WASI runtime used when WASM_EXEC is unset (default: wasmtime)
#   WASM      module path (default: ../water.wasm)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
module=${WASM:-$root/water.wasm}
skiplist="$here/wasm-skip.txt"

if [ -n "${WASM_EXEC:-}" ]; then
    exec_cmd=$WASM_EXEC
else
    runtime=${WASMTIME:-wasmtime}
    command -v "$runtime" >/dev/null 2>&1 || { echo "no runtime: set WASMTIME=<path> or WASM_EXEC=<command>"; exit 1; }
    exec_cmd="$runtime run --dir . --dir /tmp"
fi
[ -f "$module" ] || { echo "module '$module' not found (run: make wasm)"; exit 1; }

pass=0
fail=0
skip=0

for input in "$here"/*.h2o; do
    [ -e "$input" ] || { echo "no tests found"; exit 1; }
    name=$(basename "$input" .h2o)
    expected="$here/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "SKIP $name (no .expected file)"
        continue
    fi
    reason=""
    if [ -f "$skiplist" ]; then
        reason=$(awk -v n="$name" '$1==n {$1=""; sub(/^[ \t]+/, ""); print; exit}' "$skiplist")
    fi
    if [ -n "$reason" ]; then
        skip=$((skip + 1))
        printf "  SKIP %s (%s)\n" "$name" "$reason"
        continue
    fi
    actual=$(mktemp "${TMPDIR:-/tmp}/lfwasm.XXXXXX")
    # Piped stdin defaults to batch mode (no banner, no prompt), matching how
    # the .expected files were captured by the native harness. Preopen the repo
    # root (guest ".") for relative loads and /tmp for scratch files, so file
    # I/O tests get the same access the native harness has.
    (cd "$root" && $exec_cmd "$module" -b < "$input") > "$actual" 2>&1
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
echo "$pass passed, $fail failed, $skip skipped (wasm)"
[ "$fail" -eq 0 ]
