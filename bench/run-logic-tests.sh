#!/usr/bin/env bash
#
# Run the logic-subsystem benchmarks. Each bench/logic/<name>.l4 is run under
# logicforth; if a <name>.pl reference exists and swipl is installed, the
# SWI-Prolog version is run too and a logicforth/swipl ratio is reported (both
# print a self-timed "elapsed:" line). logicforth's amb search (logic.l4's 4th
# section) runs long, so the logicforth run is bounded by LF_TIMEOUT (the
# unification "elapsed:" prints before it, so it is still captured).
#
#   bench/run-logic-tests.sh            # run all
#   bench/run-logic-tests.sh nreverse   # by name
#
# Env: LF_TIMEOUT  seconds to bound each logicforth run (default 60)

set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/logicforth"
swipl_bin=$(command -v swipl || true)
lf_timeout=${LF_TIMEOUT:-60}
log() { printf '%s\n' "$*" >&2; }

log "building logicforth..."
(cd "$root" && make) >&2 || { log "build failed"; exit 1; }

elapsed_of() { grep -i '^elapsed:' | grep -oE '[0-9]+\.[0-9]+' | head -1; }

tests=("$@")
if [ "${#tests[@]}" -eq 0 ]; then
	tests=( $(cd "$here/logic" && ls *.l4 2>/dev/null | sed 's/\.l4$//') )
fi

for name in "${tests[@]}"; do
	l4="$here/logic/$name.l4"
	pl="$here/logic/$name.pl"
	[ -f "$l4" ] || { log "SKIP $name (no $l4)"; continue; }

	printf '\n========== %s ==========\n' "$name"
	lf_out=$( cd "$root" && timeout "$lf_timeout" "$bin" -b < "$l4" 2>&1 )
	printf '%s\n' "$lf_out"
	lf_t=$(printf '%s\n' "$lf_out" | elapsed_of)

	if [ -n "$swipl_bin" ] && [ -f "$pl" ]; then
		printf -- '----- swipl %s.pl -----\n' "$name"
		pl_out=$( "$swipl_bin" "$pl" 2>&1 )
		printf '%s\n' "$pl_out"
		pl_t=$(printf '%s\n' "$pl_out" | elapsed_of)
		if [ -n "${lf_t:-}" ] && [ -n "${pl_t:-}" ]; then
			rel=$(awk -v a="$lf_t" -v b="$pl_t" 'BEGIN{ if (a<=b) printf "logicforth %.2fx faster", b/a; else printf "swipl %.2fx faster", a/b }')
			printf -- '---------- %s: logicforth %ss  vs  swipl %ss  (%s) ----------\n' \
				"$name" "$lf_t" "$pl_t" "$rel"
		fi
	fi
done

log "done."
