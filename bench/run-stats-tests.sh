#!/usr/bin/env bash
#
# Run the heavy statistics benchmarks (water-only; serial vs parallel bootstrap).
# Ensures the synthetic dataset exists (generated once via gen-logistic-sim.h2o),
# then runs each bench/stats/*.h2o benchmark with wall-clock time and exit status.
#
#   bench/run-stats-tests.sh                  # run all
#   bench/run-stats-tests.sh bootstrap-logistic

set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/water"
log() { printf '%s\n' "$*" >&2; }

log "building water..."
(cd "$root" && make) >&2 || { log "build failed"; exit 1; }

# the benchmarks read data/logistic-sim.tsv; generate it once if absent.
ensure_sim() {
	[ -s "$root/data/logistic-sim.tsv" ] && return 0
	log "  generating data/logistic-sim.tsv (1M rows, one-time)..."
	( cd "$root" && "$bin" -b < "$here/stats/gen-logistic-sim.h2o" ) >&2 || { log "  generation failed"; return 1; }
}

tests=("$@")
if [ "${#tests[@]}" -eq 0 ]; then
	tests=(bootstrap-logistic)
fi

ensure_sim || { log "cannot run stats benchmarks without the dataset"; exit 1; }

for name in "${tests[@]}"; do
	file="$here/stats/$name.h2o"
	[ -f "$file" ] || { log "SKIP $name (no $file)"; continue; }
	printf '\n========== %s ==========\n' "$name"
	start=$(date +%s)
	( cd "$root" && "$bin" -b < "$file" )
	status=$?
	end=$(date +%s)
	printf -- '---------- %s: exit %d in %ds ----------\n' "$name" "$status" "$((end - start))"
done

log "done."
