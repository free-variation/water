#!/bin/sh
# Disassemble every word defined in each bench/**.h2o file.
#
# For each .h2o under ./bench: extract its colon-definition names, load the
# file's definitions (the trailing benchmark run is stripped), and run
# see-compiled on each word, dumping the disassembly to stdout. Useful for
# auditing which sequences fused across the real benchmarks.

root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/water"

# The benchmark runner injects these per-run params; the bench words reference
# them, so define them (dummy values) or the files won't compile.
prelude='variable LOOPS 1.0 to LOOPS
variable CYCLES 1.0 to CYCLES
variable ITERATIONS 1.0 to ITERATIONS
variable SAMPLES 1.0 to SAMPLES
variable WORKERS 1.0 to WORKERS'

find "$root/bench" -name '*.h2o' | sort | while IFS= read -r file; do
	echo "================================================================"
	echo "# $file"
	echo "================================================================"

	# names of words defined with `: name ...`
	words=$(grep -oE '^:[[:space:]]+[^[:space:]]+' "$file" | sed -E 's/^:[[:space:]]*//')

	# load definitions only: keep through the last line containing ';',
	# dropping the trailing top-level benchmark invocation and `bye`.
	last=$(grep -n ';' "$file" | tail -1 | cut -d: -f1)
	[ -z "$last" ] && last=$(wc -l < "$file")

	{
		echo "$prelude"
		head -n "$last" "$file"
		for w in $words; do
			echo "' $w see-compiled"
			echo cr
		done
		echo bye
	} | "$bin" -b
done
