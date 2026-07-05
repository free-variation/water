#!/usr/bin/env bash
#
# Run the logic-subsystem benchmarks. Each bench/logic/<name>.h2o is run under
# water; if a <name>.pl reference exists and swipl is installed, the
# SWI-Prolog version is run too and a water/swipl ratio is reported (both
# print a self-timed "elapsed:" line). water's amb search (logic.h2o's 4th
# section) runs long, so the water run is bounded by LF_TIMEOUT (the
# unification "elapsed:" prints before it, so it is still captured).
#
#   bench/run-logic-tests.sh            # run all
#   bench/run-logic-tests.sh nreverse   # by name
#
# Env: LF_TIMEOUT  seconds to bound each water run (default 60)

set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/water"
swipl_bin=$(command -v swipl || true)
h2o_timeout=${LF_TIMEOUT:-60}
log() { printf '%s\n' "$*" >&2; }

log "building water..."
(cd "$root" && make) >&2 || { log "build failed"; exit 1; }

elapsed_of() { grep -i '^elapsed:' | grep -oE '[0-9]+\.[0-9]+' | head -1; }

tests=("$@")
if [ "${#tests[@]}" -eq 0 ]; then
	tests=( $(cd "$here/logic" && ls *.h2o 2>/dev/null | sed 's/\.h2o$//') )
fi

for name in "${tests[@]}"; do
	l4="$here/logic/$name.h2o"
	pl="$here/logic/$name.pl"
	[ -f "$l4" ] || { log "SKIP $name (no $l4)"; continue; }

	printf '\n========== %s ==========\n' "$name"
	h2o_out=$( cd "$root" && timeout "$h2o_timeout" "$bin" -b < "$l4" 2>&1 )
	printf '%s\n' "$h2o_out"
	h2o_t=$(printf '%s\n' "$h2o_out" | elapsed_of)

	if [ -n "$swipl_bin" ] && [ -f "$pl" ]; then
		printf -- '----- swipl %s.pl -----\n' "$name"
		pl_out=$( "$swipl_bin" "$pl" 2>&1 )
		printf '%s\n' "$pl_out"
		pl_t=$(printf '%s\n' "$pl_out" | elapsed_of)
		if [ -n "${h2o_t:-}" ] && [ -n "${pl_t:-}" ]; then
			rel=$(awk -v a="$h2o_t" -v b="$pl_t" 'BEGIN{ if (a<=b) printf "water %.2fx faster", b/a; else printf "swipl %.2fx faster", a/b }')
			printf -- '---------- %s: water %ss  vs  swipl %ss  (%s) ----------\n' \
				"$name" "$h2o_t" "$pl_t" "$rel"
		fi
	fi
done

log "done."
