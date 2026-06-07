#!/usr/bin/env bash
#
# Run the logicforth benchmark suite against CPython and emit a markdown
# report on stdout (the same shape as bench/baseline-post-*.md). All progress
# and build output goes to stderr, so the report can be captured cleanly:
#
#   bench/run-benchmarks.sh > bench/baseline-$(date +%Y%m%d).md
#
# Environment overrides:
#   PYTHON        python interpreter            (default: python3.14)
#   REPS          logicforth reps per bench     (default: 5)
#   REPS_PY       python reps per bench         (default: 3)
#   SKIP_LEIBNIZ  set to 1 to skip leibniz      (it is the slow one: ~min)
#
# Problem sizes match the committed pyperformance ports.

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/logicforth"

python=${PYTHON:-python3.14}
reps=${REPS:-5}
reps_py=${REPS_PY:-3}
skip_leibniz=${SKIP_LEIBNIZ:-0}

nbody_steps=20000
spectral_loops=50
scimark_lu_cycles=100
nqueens_n=8
fannkuch_n=9
leibniz_rounds=1000000000
leibniz_url="https://raw.githubusercontent.com/niklas-heer/speed-comparison/master/src/leibniz.py"
leibniz_r_url="https://raw.githubusercontent.com/niklas-heer/speed-comparison/master/src/leibniz.r"

work=$(mktemp -d "${TMPDIR:-/tmp}/lfbench.XXXXXX")
trap 'rm -rf "$work"' EXIT

log() { printf '%s\n' "$*" >&2; }

# --- build -----------------------------------------------------------------
log "building logicforth..."
(cd "$root" && make) >&2 || { log "build failed"; exit 1; }

# --- helpers ---------------------------------------------------------------

# median of numbers on stdin (average of the two middle values when even)
median() {
	sort -n | awk '{ a[NR] = $1 }
		END {
			if (NR == 0) { print "nan"; exit }
			if (NR % 2) { print a[(NR + 1) / 2] }
			else { print (a[NR / 2] + a[NR / 2 + 1]) / 2 }
		}'
}

# first decimal number on the "elapsed" line(s) of stdin
elapsed_of() { grep -i elapsed | grep -oE '[0-9]+\.[0-9]+' | head -1; }

# format seconds: 4 decimals under 1s, 3 decimals at/above
fmt_s() { awk -v v="$1" 'BEGIN { printf (v >= 1 ? "%.3f s" : "%.4f s"), v }'; }

# format milliseconds with precision scaled to magnitude
fmt_ms() {
	awk -v v="$1" 'BEGIN {
		ms = v * 1000
		if (ms < 1)       printf "%.2f ms", ms
		else if (ms < 10) printf "%.1f ms", ms
		else              printf "%.0f ms", ms
	}'
}

# python-over-logicforth ratio, e.g. 1.28× or ~37×
ratio() {
	awk -v p="$1" -v l="$2" 'BEGIN {
		r = p / l
		if (r >= 10) printf "~%.0f×", r
		else printf "%.2f×", r
	}'
}

# --- logicforth command wrappers (each prints a full bench run) ------------
lf_synth()    { "$bin" < "$here/synth.l4"; }
lf_leibniz()  { "$bin" < "$here/leibniz.l4"; }
lf_leibniz_matrix() { "$bin" < "$here/leibniz-matrix.l4"; }
lf_nqueens()  { "$bin" < "$here/nqueens.l4"; }
lf_fannkuch() { "$bin" < "$here/fannkuch.l4"; }
lf_nbody()    { { echo "variable ITERATIONS $nbody_steps to ITERATIONS"; cat "$here/nbody.l4"; } | "$bin"; }
lf_spectral() { { echo "variable ITERATIONS $spectral_loops to ITERATIONS"; cat "$here/spectral-norm.l4"; } | "$bin"; }
lf_scimark_lu() { { echo "variable ITERATIONS $scimark_lu_cycles to ITERATIONS"; cat "$here/scimark-lu.l4"; } | "$bin"; }
lf_regex_dna() { "$bin" < "$here/regex-dna.l4"; }
lf_regex_compile() { "$bin" < "$here/regex-compile.l4"; }

# --- python command wrappers -----------------------------------------------
py_synth()    { "$python" "$here/synth.py"; }
py_nqueens()  { "$python" "$here/pyperf_nqueens.py" "$nqueens_n"; }
py_fannkuch() { "$python" "$here/pyperf_fannkuch.py" "$fannkuch_n"; }
py_nbody()    { "$python" "$here/pyperf_nbody.py" "$nbody_steps"; }
py_spectral() { "$python" "$here/pyperf_spectral_norm.py" "$spectral_loops"; }
py_scimark_lu() { "$python" "$here/pyperf_scimark_lu.py" "$scimark_lu_cycles"; }
py_regex_dna() { "$python" "$here/pyperf_regex_dna.py"; }
py_regex_compile() { "$python" "$here/pyperf_regex_compile.py"; }

# Run a wrapper N times, append each run's stdout (with a separator) to a log.
run_reps() {
	local key=$1 fn=$2 n=$3 i
	: > "$work/$key.log"
	for i in $(seq 1 "$n"); do
		log "  $key: run $i/$n"
		"$fn" >> "$work/$key.log" 2>/dev/null
		printf '\n===RUNSEP===\n' >> "$work/$key.log"
	done
}

# Median elapsed across the runs recorded by run_reps for $key.
median_elapsed() {
	local key=$1
	grep -i elapsed "$work/$key.log" | grep -oE '[0-9]+\.[0-9]+' | median
}

# Median elapsed for a single synth phase across runs (e.g. phase 3).
synth_phase_elapsed() {
	local key=$1 phase=$2
	grep "phase$phase elapsed=" "$work/$key.log" | grep -oE '[0-9]+\.[0-9]+' | median
}

# Last matching result line from a bench log (for the verification section).
result_line() {
	local key=$1 pattern=$2
	grep -iE "$pattern" "$work/$key.log" | tail -1 | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'
}

# --- leibniz python reference (fetched from upstream) ----------------------
leibniz_py_elapsed=""
leibniz_py_result=""
run_leibniz_py() {
	local ref="$work/leibniz_ref.py" i
	log "fetching upstream leibniz.py reference..."
	if ! curl -fsSL "$leibniz_url" -o "$ref"; then
		log "  WARNING: could not fetch leibniz reference; skipping python leibniz"
		return 1
	fi
	echo "$leibniz_rounds" > "$work/rounds.txt"
	# the reference prints only the result, so time it externally and capture
	# both the elapsed seconds and the printed pi on each run (tab-separated)
	local times="$work/leibniz_py_times"
	: > "$times"
	for i in $(seq 1 "$reps_py"); do
		log "  leibniz(py): run $i/$reps_py"
		"$python" - "$python" "$ref" "$work" >> "$times" <<'PYEOF'
import sys, time, subprocess
interp, ref, cwd = sys.argv[1], sys.argv[2], sys.argv[3]
start = time.perf_counter()
done = subprocess.run([interp, ref], cwd=cwd, capture_output=True, text=True)
print(f"{time.perf_counter() - start:.6f}\t{done.stdout.strip()}")
PYEOF
	done
	leibniz_py_elapsed=$(cut -f1 "$times" | median)
	leibniz_py_result=$(tail -1 "$times" | cut -f2)
}

# --- leibniz R reference (the vectorized one-liner leibniz-matrix mirrors) --
# Optional: skipped cleanly when Rscript isn't installed.
leibniz_r_elapsed=""
leibniz_r_result=""
leibniz_r_version=""
run_leibniz_r() {
	command -v Rscript >/dev/null 2>&1 || { log "  Rscript not found; skipping R leibniz"; return 1; }
	local ref="$work/leibniz_ref.r" i
	log "fetching upstream leibniz.r reference..."
	if ! curl -fsSL "$leibniz_r_url" -o "$ref"; then
		log "  WARNING: could not fetch leibniz.r reference; skipping R leibniz"
		return 1
	fi
	echo "$leibniz_rounds" > "$work/rounds.txt"
	leibniz_r_version=$(Rscript --version 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
	# the reference prints only the result; time it externally, capture both
	local times="$work/leibniz_r_times"
	: > "$times"
	for i in $(seq 1 "$reps_py"); do
		log "  leibniz(R): run $i/$reps_py"
		"$python" - "Rscript" "$ref" "$work" >> "$times" <<'PYEOF'
import sys, time, subprocess
interp, ref, cwd = sys.argv[1], sys.argv[2], sys.argv[3]
start = time.perf_counter()
done = subprocess.run([interp, ref], cwd=cwd, capture_output=True, text=True)
print(f"{time.perf_counter() - start:.6f}\t{done.stdout.strip()}")
PYEOF
	done
	leibniz_r_elapsed=$(cut -f1 "$times" | median)
	leibniz_r_result=$(tail -1 "$times" | cut -f2)
}

# ===========================================================================
# Run everything
# ===========================================================================
log "logicforth: $reps reps/bench   python: $reps_py reps/bench   ($python)"

log "== synth =="
run_reps synth_lf lf_synth "$reps"
run_reps synth_py py_synth "$reps_py"

log "== nqueens =="
run_reps nqueens_lf lf_nqueens "$reps"
run_reps nqueens_py py_nqueens "$reps_py"

log "== nbody =="
run_reps nbody_lf lf_nbody "$reps"
run_reps nbody_py py_nbody "$reps_py"

log "== fannkuch =="
run_reps fannkuch_lf lf_fannkuch "$reps"
run_reps fannkuch_py py_fannkuch "$reps_py"

log "== spectral-norm =="
run_reps spectral_lf lf_spectral "$reps"
run_reps spectral_py py_spectral "$reps_py"

log "== scimark-lu =="
run_reps scimark_lu_lf lf_scimark_lu "$reps"
run_reps scimark_lu_py py_scimark_lu "$reps_py"

log "== regex-dna =="
run_reps regex_dna_lf lf_regex_dna "$reps"
run_reps regex_dna_py py_regex_dna "$reps_py"

log "== regex-compile =="
run_reps regex_compile_lf lf_regex_compile "$reps"
run_reps regex_compile_py py_regex_compile "$reps_py"

have_leibniz=0
have_leibniz_r=0
if [ "$skip_leibniz" != 1 ]; then
	log "== leibniz + leibniz-matrix (slow) =="
	run_reps leibniz_lf lf_leibniz "$reps"
	run_reps leibniz_matrix_lf lf_leibniz_matrix "$reps"
	if run_leibniz_py; then have_leibniz=1; fi
	if run_leibniz_r; then have_leibniz_r=1; fi
fi

# ===========================================================================
# Emit the report
# ===========================================================================
pyver=$("$python" --version 2>&1 | awk '{print $2}')
today=$(date +%Y-%m-%d)
uname_s=$(uname -sr)

emit() { printf '%s\n' "$*"; }

emit "# Benchmark report"
emit ""
emit "Generated by \`bench/run-benchmarks.sh\` ($reps logicforth reps, $reps_py python reps; medians)."
emit ""
emit "## Environment"
emit ""
emit "- **Host**: $uname_s"
emit "- **Compiler**: \`clang -O3 -march=native -Wall -Wextra\`"
emit "- **Python**: CPython $pyver (\`$python\`)"
emit "- **Date**: $today"
emit ""

# ---- synth table ----
emit "## synth.l4 — logicforth vs CPython $pyver"
emit ""
emit "| phase | logicforth | python | py / lf |"
emit "|:-----:|-----------:|-------:|--------:|"
synth_lf_total=0
synth_py_total=0
for phase in 1 2 3 4 5 6; do
	lf=$(synth_phase_elapsed synth_lf "$phase")
	py=$(synth_phase_elapsed synth_py "$phase")
	synth_lf_total=$(awk -v a="$synth_lf_total" -v b="$lf" 'BEGIN{print a+b}')
	synth_py_total=$(awk -v a="$synth_py_total" -v b="$py" 'BEGIN{print a+b}')
	emit "| $phase | $(fmt_ms "$lf") | $(fmt_ms "$py") | $(ratio "$py" "$lf") |"
done
emit "| total | $(fmt_ms "$synth_lf_total") | $(fmt_ms "$synth_py_total") | $(ratio "$synth_py_total" "$synth_lf_total") |"
emit ""

# ---- standalone table ----
emit "## Standalone benchmarks — logicforth vs CPython $pyver"
emit ""
emit "| benchmark | size | logicforth | python | py / lf |"
emit "|:----------|:-----|-----------:|-------:|--------:|"

row() {
	# $1 label  $2 size  $3 lf_key  $4 py_elapsed_or_key  $5 (py is literal?)
	local label=$1 size=$2 lf_key=$3
	local lf py
	lf=$(median_elapsed "$lf_key")
	py=$4
	emit "| $label | $size | $(fmt_s "$lf") | $(fmt_s "$py") | $(ratio "$py" "$lf") |"
}

if [ "$have_leibniz" = 1 ]; then
	row "leibniz" "${leibniz_rounds} iterations" leibniz_lf "$leibniz_py_elapsed"
	row "leibniz-matrix" "${leibniz_rounds}, vectorized" leibniz_matrix_lf "$leibniz_py_elapsed"
fi
row "nqueens" "N = $nqueens_n" nqueens_lf "$(median_elapsed nqueens_py)"
row "nbody" "${nbody_steps} steps" nbody_lf "$(median_elapsed nbody_py)"
row "fannkuch" "N = $fannkuch_n" fannkuch_lf "$(median_elapsed fannkuch_py)"
row "spectral-norm" "N = 130, ${spectral_loops}×" spectral_lf "$(median_elapsed spectral_py)"
row "scimark-lu" "N=100, ${scimark_lu_cycles}×" scimark_lu_lf "$(median_elapsed scimark_lu_py)"
row "regex-dna" "100K → 1M" regex_dna_lf "$(median_elapsed regex_dna_py)"
row "regex-compile" "239 patterns, cold" regex_compile_lf "$(median_elapsed regex_compile_py)"
emit ""

# ---- R reference for the vectorized variant ----
if [ "$skip_leibniz" != 1 ] && [ "$have_leibniz_r" = 1 ]; then
	lf_matrix=$(median_elapsed leibniz_matrix_lf)
	emit "**Vectorized reference (R).** logicforth's leibniz-matrix mirrors the R"
	emit "one-liner \`sum(4 / seq.int(...))\`. R $leibniz_r_version runs it in"
	emit "$(fmt_s "$leibniz_r_elapsed") (π = $leibniz_r_result); logicforth's leibniz-matrix is"
	emit "$(ratio "$lf_matrix" "$leibniz_r_elapsed") that at $(fmt_s "$lf_matrix")."
	emit ""
fi

# ---- verification ----
emit "## Verification (results must match)"
emit ""
emit "| benchmark | logicforth | python |"
emit "|:----------|:-----------|:-------|"
emit "| nqueens | $(result_line nqueens_lf 'solutions') | $(result_line nqueens_py 'solutions') |"
emit "| nbody | $(result_line nbody_lf 'final energy') | $(result_line nbody_py 'final energy') |"
emit "| fannkuch | $(result_line fannkuch_lf 'max flips') | $(result_line fannkuch_py 'max flips') |"
emit "| spectral-norm | $(result_line spectral_lf 'estimate') | $(result_line spectral_py 'estimate') |"
emit "| scimark-lu | $(result_line scimark_lu_lf 'checksum') | $(result_line scimark_lu_py 'checksum') |"
emit "| regex-dna | $(result_line regex_dna_lf 'result:') | $(result_line regex_dna_py 'result:') |"
emit "| regex-compile | $(result_line regex_compile_lf 'patterns:') | $(result_line regex_compile_py 'patterns:') |"
if [ "$have_leibniz" = 1 ]; then
	emit "| leibniz | $(result_line leibniz_lf 'pi:') | pi = $leibniz_py_result |"
	emit "| leibniz-matrix | $(result_line leibniz_matrix_lf 'pi:') | pi = $leibniz_py_result |"
fi
emit ""

log "done."
