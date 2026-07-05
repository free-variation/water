#!/usr/bin/env bash
#
# Run the logicforth load / stress tests — the bench/ files that work the
# system hard rather than compare against CPython (run-benchmarks.sh does the
# comparisons). Each is run as `logicforth -b < file` from the repo root, with
# a header, wall-clock time, and exit status. Test output goes to stdout;
# build/progress goes to stderr.
#
#   bench/run-load-tests.sh                 # run all
#   bench/run-load-tests.sh image-load sqlite-load   # run a subset, by name
#
# Tests:
#   image-load       save-image / load-image round-trip over a 100k-object heap
#   sqlite-load      db-query / create-index materializing 1M row frames
#   parallel-stress  pmap / pfilter / pmap-reduce over a large domain, 500 rounds
#   extreme-load     drives every structure to and past its limit; fills the
#                    64M object table (~7 GB) unless MAX_OBJECTS caps it
#   select-load      profiling load: select-values over a ~53 MB JSON fixture
#                    (auto-generates bench/fixtures/big.json; ~1e9 queries — the long one)
#
# Env:
#   MAX_OBJECTS   object-table ceiling passed to extreme-load (default: full 64M)
#   PYTHON        python used to generate big.json (default: python3)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/logicforth"
python=${PYTHON:-python3}

log() { printf '%s\n' "$*" >&2; }

log "building logicforth..."
(cd "$root" && make) >&2 || { log "build failed"; exit 1; }

# select-load reads "bench/fixtures/big.json" by relative path; generate it once if absent.
ensure_big_json() {
	[ -s "$root/bench/fixtures/big.json" ] && return 0
	command -v "$python" >/dev/null 2>&1 || { log "  $python not found; cannot generate big.json"; return 1; }
	log "  generating bench/fixtures/big.json (~53 MB, one-time)..."
	"$python" "$here/fixtures/gen-big-json.py" "$root/bench/fixtures/big.json" >&2 || { log "  big.json generation failed"; return 1; }
}

run_one() {
	local name=$1
	local file="$here/load/$name.l4"
	[ -f "$file" ] || { log "SKIP $name (no $file)"; return; }

	if [ "$name" = select-load ]; then
		ensure_big_json || { log "SKIP select-load (no fixture)"; return; }
	fi

	printf '\n========== %s ==========\n' "$name"
	local start end status
	start=$(date +%s)
	interrupted=0
	if [ "$name" = extreme-load ] && [ -n "${MAX_OBJECTS:-}" ]; then
		( cd "$root" && "$bin" -b --max-objects "$MAX_OBJECTS" < "$file" )
	else
		( cd "$root" && "$bin" -b < "$file" )
	fi
	status=$?
	[ "$interrupted" = 1 ] && status=$int_status
	end=$(date +%s)
	printf -- '---------- %s: exit %d in %ds ----------\n' "$name" "$status" "$((end - start))"
}

tests=("$@")
if [ "${#tests[@]}" -eq 0 ]; then
	# Finite tests first; the open-ended select-load (stopped with Ctrl-C) runs last.
	tests=()
	for t in $(cd "$here/load" && ls *.l4 2>/dev/null | sed 's/\.l4$//'); do
		[ "$t" = select-load ] || tests+=("$t")
	done
	[ -f "$here/load/select-load.l4" ] && tests+=(select-load)
fi

# An open-ended test (e.g. select-load) is meant to be stopped with Ctrl-C.
# Trap SIGINT so it ends only the current test — recording its status and
# moving on — rather than killing the whole script.
interrupted=0
int_status=0
trap 'int_status=$?; interrupted=1' INT

for t in "${tests[@]}"; do
	run_one "$t"
done

log "done."
