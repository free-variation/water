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
# crypto-pyaes needs the pure-Python `pyaes` module; prefer the repo .venv
# (where it is installed) over the default interpreter. Override with CRYPTO_PYTHON.
if [ -n "${CRYPTO_PYTHON:-}" ]; then
	crypto_python=$CRYPTO_PYTHON
elif [ -x "$root/.venv/bin/python" ]; then
	crypto_python="$root/.venv/bin/python"
else
	crypto_python=$python
fi
reps=${REPS:-5}
reps_py=${REPS_PY:-3}
# regex-compile is an inherently single-shot cold measure (~1 ms; the cache
# warms after pass 1, so it can't be looped in-process). Sample it across many
# fresh processes instead, for a tighter median.
reps_compile=${REPS_COMPILE:-25}
skip_leibniz=${SKIP_LEIBNIZ:-0}

nbody_steps=20000
raytrace_loops=10
float_points=100000
float_repeat=20
crypto_loops=10
spectral_loops=50
scimark_lu_cycles=100
nqueens_n=8
fannkuch_n=9
scimark_sor_loops=100
scimark_sparse_cycles=500
fft_loops=5
fft_cycles=50
barnes_iterations=50
barnes_loops=2
montecarlo_samples=1000000
montecarlo_loops=3
meteor_loops=10
hexiom_loops=50
leibniz_rounds=1000000000

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
lf_leibniz()  { "$bin" < "$here/pyperformance/leibniz.l4"; }
lf_leibniz_matrix() { "$bin" < "$here/variants/leibniz-matrix.l4"; }
lf_leibniz_parallel() { "$bin" < "$here/variants/leibniz-parallel.l4"; }
lf_nqueens()  { "$bin" < "$here/pyperformance/nqueens.l4"; }
lf_nqueens_iter() { "$bin" < "$here/variants/nqueens-iter.l4"; }
lf_fannkuch() { "$bin" < "$here/pyperformance/fannkuch.l4"; }
lf_nbody()    { { echo "variable ITERATIONS $nbody_steps to ITERATIONS"; cat "$here/pyperformance/nbody.l4"; } | "$bin"; }
lf_raytrace() { { echo "variable LOOPS $raytrace_loops to LOOPS"; cat "$here/pyperformance/raytrace.l4"; } | "$bin"; }
lf_float()    { "$bin" < "$here/pyperformance/float.l4"; }
lf_crypto()   { "$bin" < "$here/pyperformance/crypto-pyaes.l4"; }
lf_spectral() { { echo "variable ITERATIONS $spectral_loops to ITERATIONS"; cat "$here/pyperformance/spectral-norm.l4"; } | "$bin"; }
lf_spectral_matrix() { { echo "variable ITERATIONS $spectral_loops to ITERATIONS"; cat "$here/variants/spectral-norm-matrix.l4"; } | "$bin"; }
lf_scimark_lu() { { echo "variable ITERATIONS $scimark_lu_cycles to ITERATIONS"; cat "$here/pyperformance/scimark-lu.l4"; } | "$bin"; }
lf_scimark_sor() { { echo "variable ITERATIONS $scimark_sor_loops to ITERATIONS"; cat "$here/pyperformance/scimark-sor.l4"; } | "$bin"; }
lf_scimark_sparse() { { echo "variable CYCLES $scimark_sparse_cycles to CYCLES"; cat "$here/pyperformance/scimark-sparse.l4"; } | "$bin"; }
lf_scimark_fft() { { echo "variable LOOPS $fft_loops to LOOPS"; echo "variable CYCLES $fft_cycles to CYCLES"; cat "$here/pyperformance/scimark-fft.l4"; } | "$bin"; }
lf_barnes()   { { echo "variable ITERATIONS $barnes_iterations to ITERATIONS"; echo "variable LOOPS $barnes_loops to LOOPS"; cat "$here/pyperformance/barnes-hut.l4"; } | "$bin"; }
lf_scimark_mc() { { echo "variable SAMPLES $montecarlo_samples to SAMPLES"; echo "variable LOOPS $montecarlo_loops to LOOPS"; cat "$here/pyperformance/scimark-montecarlo.l4"; } | "$bin"; }
lf_montecarlo_par() { { echo "variable SAMPLES $((montecarlo_samples * montecarlo_loops)) to SAMPLES"; echo "variable WORKERS 10 to WORKERS"; cat "$here/variants/monte-carlo-parallel.l4"; } | "$bin"; }
lf_meteor()   { { echo "variable LOOPS $meteor_loops to LOOPS"; cat "$here/pyperformance/meteor.l4"; } | "$bin"; }
lf_hexiom()   { { echo "variable LOOPS $hexiom_loops to LOOPS"; cat "$here/pyperformance/hexiom.l4"; } | "$bin"; }
lf_regex_dna() { "$bin" < "$here/pyperformance/regex-dna.l4"; }
lf_regex_compile() { "$bin" < "$here/pyperformance/regex-compile.l4"; }
lf_regex_effbot() { "$bin" < "$here/pyperformance/regex-effbot.l4"; }
lf_regex_v8() { "$bin" < "$here/pyperformance/regex-v8.l4"; }
lf_deepcopy() { "$bin" < "$here/pyperformance/deepcopy.l4"; }
lf_json_loads() { "$bin" < "$here/pyperformance/json-loads.l4"; }
lf_json_dumps() { "$bin" < "$here/pyperformance/json-dumps.l4"; }

# --- python command wrappers -----------------------------------------------
py_nqueens()  { "$python" "$here/pyperformance/pyperf_nqueens.py" "$nqueens_n"; }
py_fannkuch() { "$python" "$here/pyperformance/pyperf_fannkuch.py" "$fannkuch_n"; }
py_nbody()    { "$python" "$here/pyperformance/pyperf_nbody.py" "$nbody_steps"; }
py_raytrace() { "$python" "$here/pyperformance/pyperf_raytrace.py" "$raytrace_loops"; }
py_float()    { "$python" "$here/pyperformance/pyperf_float.py" "$float_points" "$float_repeat"; }
py_crypto()   { "$crypto_python" "$here/pyperformance/pyperf_crypto_pyaes.py" "$crypto_loops"; }
py_spectral() { "$python" "$here/pyperformance/pyperf_spectral_norm.py" "$spectral_loops"; }
py_scimark_lu() { "$python" "$here/pyperformance/pyperf_scimark_lu.py" "$scimark_lu_cycles"; }
py_scimark_sor() { "$python" "$here/pyperformance/pyperf_scimark_sor.py" "$scimark_sor_loops"; }
py_scimark_sparse() { "$python" "$here/pyperformance/pyperf_scimark_sparse.py" "$scimark_sparse_cycles"; }
py_scimark_fft() { "$python" "$here/pyperformance/pyperf_scimark_fft.py" "$fft_loops" "$fft_cycles"; }
py_barnes()   { "$python" "$here/pyperformance/pyperf_barnes_hut.py" "$barnes_loops" "$barnes_iterations"; }
py_scimark_mc() { "$python" "$here/pyperformance/pyperf_scimark_montecarlo.py" "$montecarlo_samples" "$montecarlo_loops"; }
py_meteor()   { "$python" "$here/pyperformance/pyperf_meteor.py" "$meteor_loops"; }
py_hexiom()   { "$python" "$here/pyperformance/pyperf_hexiom.py" "$hexiom_loops"; }
py_regex_dna() { "$python" "$here/pyperformance/pyperf_regex_dna.py"; }
py_regex_compile() { "$python" "$here/pyperformance/pyperf_regex_compile.py"; }
py_regex_effbot() { "$python" "$here/pyperformance/pyperf_regex_effbot.py"; }
py_regex_v8() { "$python" "$here/pyperformance/pyperf_regex_v8.py"; }
py_deepcopy() { "$python" "$here/pyperformance/pyperf_deepcopy.py"; }
py_json_loads() { "$python" "$here/pyperformance/pyperf_json_loads.py"; }
py_json_dumps() { "$python" "$here/pyperformance/pyperf_json_dumps.py"; }

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

# Last matching result line from a bench log (for the verification section).
result_line() {
	local key=$1 pattern=$2
	grep -iE "$pattern" "$work/$key.log" | tail -1 | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'
}

# --- leibniz python + R references (cached) --------------------------------
# Python leibniz (~40s) and R leibniz are not measured live: Python is slow and
# R is often absent. These reference numbers are cached from a prior full run.
# Refresh by hand if the host or interpreter versions change.
leibniz_py_elapsed=42.258
leibniz_py_result=3.1415926525880504

leibniz_r_elapsed=1.720
leibniz_r_result=3.1415926525897171
leibniz_r_version=4.5.2

# ===========================================================================
# Run everything
# ===========================================================================
log "logicforth: $reps reps/bench   python: $reps_py reps/bench   ($python)"

log "== nqueens =="
run_reps nqueens_lf lf_nqueens "$reps"
run_reps nqueens_iter_lf lf_nqueens_iter "$reps"
run_reps nqueens_py py_nqueens "$reps_py"

log "== nbody =="
run_reps nbody_lf lf_nbody "$reps"
run_reps nbody_py py_nbody "$reps_py"

log "== raytrace =="
run_reps raytrace_lf lf_raytrace "$reps"
run_reps raytrace_py py_raytrace "$reps_py"

log "== float =="
run_reps float_lf lf_float "$reps"
run_reps float_py py_float "$reps_py"

log "== crypto-pyaes =="
run_reps crypto_lf lf_crypto "$reps"
run_reps crypto_py py_crypto "$reps_py"

log "== fannkuch =="
run_reps fannkuch_lf lf_fannkuch "$reps"
run_reps fannkuch_py py_fannkuch "$reps_py"

log "== spectral-norm =="
run_reps spectral_lf lf_spectral "$reps"
run_reps spectral_matrix_lf lf_spectral_matrix "$reps"
run_reps spectral_py py_spectral "$reps_py"

log "== scimark-lu =="
run_reps scimark_lu_lf lf_scimark_lu "$reps"
run_reps scimark_lu_py py_scimark_lu "$reps_py"

log "== scimark-sparse =="
run_reps scimark_sparse_lf lf_scimark_sparse "$reps"
run_reps scimark_sparse_py py_scimark_sparse "$reps_py"

log "== scimark-fft =="
run_reps scimark_fft_lf lf_scimark_fft "$reps"
run_reps scimark_fft_py py_scimark_fft "$reps_py"

log "== barnes-hut =="
run_reps barnes_lf lf_barnes "$reps"
run_reps barnes_py py_barnes "$reps_py"

log "== scimark-sor =="
run_reps scimark_sor_lf lf_scimark_sor "$reps"
run_reps scimark_sor_py py_scimark_sor "$reps_py"

log "== scimark-montecarlo =="
run_reps scimark_mc_lf lf_scimark_mc "$reps"
run_reps montecarlo_par_lf lf_montecarlo_par "$reps"
run_reps scimark_mc_py py_scimark_mc "$reps_py"

log "== meteor =="
run_reps meteor_lf lf_meteor "$reps"
run_reps meteor_py py_meteor "$reps_py"

log "== hexiom =="
run_reps hexiom_lf lf_hexiom "$reps"
run_reps hexiom_py py_hexiom "$reps_py"

log "== regex-dna =="
run_reps regex_dna_lf lf_regex_dna "$reps"
run_reps regex_dna_py py_regex_dna "$reps_py"

log "== regex-compile =="
run_reps regex_compile_lf lf_regex_compile "$reps_compile"
run_reps regex_compile_py py_regex_compile "$reps_compile"

log "== regex-effbot =="
run_reps regex_effbot_lf lf_regex_effbot "$reps"
run_reps regex_effbot_py py_regex_effbot "$reps_py"

log "== regex-v8 =="
run_reps regex_v8_lf lf_regex_v8 "$reps"
run_reps regex_v8_py py_regex_v8 "$reps_py"

log "== deepcopy =="
run_reps deepcopy_lf lf_deepcopy "$reps"
run_reps deepcopy_py py_deepcopy "$reps_py"

log "== json-loads =="
run_reps json_loads_lf lf_json_loads "$reps"
run_reps json_loads_py py_json_loads "$reps_py"

log "== json-dumps =="
run_reps json_dumps_lf lf_json_dumps "$reps"
run_reps json_dumps_py py_json_dumps "$reps_py"

have_leibniz=0
have_leibniz_r=0
if [ "$skip_leibniz" != 1 ]; then
	log "== leibniz + leibniz-matrix (slow; python/R refs are cached) =="
	run_reps leibniz_lf lf_leibniz "$reps"
	run_reps leibniz_matrix_lf lf_leibniz_matrix "$reps"
	run_reps leibniz_parallel_lf lf_leibniz_parallel "$reps"
	have_leibniz=1
	have_leibniz_r=1
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
	row "leibniz-parallel" "${leibniz_rounds}, pmap" leibniz_parallel_lf "$leibniz_py_elapsed"
fi
row "nqueens" "N = $nqueens_n" nqueens_lf "$(median_elapsed nqueens_py)"
row "nqueens-iter" "N = $nqueens_n" nqueens_iter_lf "$(median_elapsed nqueens_py)"
row "nbody" "${nbody_steps} steps" nbody_lf "$(median_elapsed nbody_py)"
row "raytrace" "${raytrace_loops}× 100×100" raytrace_lf "$(median_elapsed raytrace_py)"
row "float" "${float_points} pts × ${float_repeat}" float_lf "$(median_elapsed float_py)"
row "crypto-pyaes" "8192 B, ${crypto_loops}× enc+dec" crypto_lf "$(median_elapsed crypto_py)"
row "fannkuch" "N = $fannkuch_n" fannkuch_lf "$(median_elapsed fannkuch_py)"
row "spectral-norm" "N = 130, ${spectral_loops}×" spectral_lf "$(median_elapsed spectral_py)"
row "spectral-norm-matrix" "N = 130, ${spectral_loops}×" spectral_matrix_lf "$(median_elapsed spectral_py)"
row "scimark-lu" "N=100, ${scimark_lu_cycles}×" scimark_lu_lf "$(median_elapsed scimark_lu_py)"
row "scimark-sparse" "N=1000, ${scimark_sparse_cycles}×" scimark_sparse_lf "$(median_elapsed scimark_sparse_py)"
row "scimark-fft" "N=1024, ${fft_loops}×${fft_cycles}" scimark_fft_lf "$(median_elapsed scimark_fft_py)"
row "barnes-hut" "200 bodies, ${barnes_loops}×${barnes_iterations}" barnes_lf "$(median_elapsed barnes_py)"
row "scimark-sor" "N=100, 10 cyc × ${scimark_sor_loops}" scimark_sor_lf "$(median_elapsed scimark_sor_py)"
row "scimark-montecarlo" "${montecarlo_samples} × ${montecarlo_loops}" scimark_mc_lf "$(median_elapsed scimark_mc_py)"
row "montecarlo-parallel" "$((montecarlo_samples * montecarlo_loops)) tot, pmap 10w" montecarlo_par_lf "$(median_elapsed scimark_mc_py)"
row "meteor" "${meteor_loops} solves" meteor_lf "$(median_elapsed meteor_py)"
row "hexiom" "level 25, ${hexiom_loops} solves" hexiom_lf "$(median_elapsed hexiom_py)"
row "regex-dna" "100K → 1M" regex_dna_lf "$(median_elapsed regex_dna_py)"
row "regex-compile" "239 patterns, cold" regex_compile_lf "$(median_elapsed regex_compile_py)"
row "regex-effbot" "21 pat × 0..10k" regex_effbot_lf "$(median_elapsed regex_effbot_py)"
row "regex-v8" "12 blocks, browser trace" regex_v8_lf "$(median_elapsed regex_v8_py)"
row "deepcopy" "N=20000, 60 copies/N" deepcopy_lf "$(median_elapsed deepcopy_py)"
row "json-loads" "222k parses" json_loads_lf "$(median_elapsed json_loads_py)"
row "json-dumps" "EMPTY/SIMPLE/NESTED/HUGE ×250" json_dumps_lf "$(median_elapsed json_dumps_py)"
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
emit "| raytrace | $(result_line raytrace_lf 'checksum') | $(result_line raytrace_py 'checksum') |"
emit "| float | $(result_line float_lf 'result:') | $(result_line float_py 'result:') |"
emit "| crypto-pyaes | $(result_line crypto_lf 'checksum:') | $(result_line crypto_py 'checksum:') |"
emit "| fannkuch | $(result_line fannkuch_lf 'max flips') | $(result_line fannkuch_py 'max flips') |"
emit "| spectral-norm | $(result_line spectral_lf 'estimate') | $(result_line spectral_py 'estimate') |"
emit "| scimark-lu | $(result_line scimark_lu_lf 'checksum') | $(result_line scimark_lu_py 'checksum') |"
emit "| scimark-sparse | $(result_line scimark_sparse_lf 'checksum') | $(result_line scimark_sparse_py 'checksum') |"
emit "| scimark-fft | $(result_line scimark_fft_lf 'checksum') | $(result_line scimark_fft_py 'checksum') |"
emit "| barnes-hut | $(result_line barnes_lf 'final energy') | $(result_line barnes_py 'final energy') |"
emit "| scimark-sor | $(result_line scimark_sor_lf 'checksum') | $(result_line scimark_sor_py 'checksum') |"
emit "| scimark-montecarlo | $(result_line scimark_mc_lf 'estimate') | $(result_line scimark_mc_py 'estimate') |"
emit "| meteor | $(result_line meteor_lf 'last:') | $(result_line meteor_py 'last:') |"
emit "| hexiom | $(result_line hexiom_lf 'signature') | $(result_line hexiom_py 'signature') |"
emit "| regex-dna | $(result_line regex_dna_lf 'result:') | $(result_line regex_dna_py 'result:') |"
emit "| regex-compile | $(result_line regex_compile_lf 'patterns:') | $(result_line regex_compile_py 'patterns:') |"
emit "| regex-effbot | $(result_line regex_effbot_lf 'matches:') | $(result_line regex_effbot_py 'matches:') |"
emit "| regex-v8 | $(result_line regex_v8_lf 'checksum:') | $(result_line regex_v8_py 'checksum:') |"
emit "| deepcopy | $(result_line deepcopy_lf 'equal:') | $(result_line deepcopy_py 'equal:') |"
emit "| json-loads | $(result_line json_loads_lf 'bytes:') | $(result_line json_loads_py 'bytes:') |"
emit "| json-dumps | $(result_line json_dumps_lf 'bytes:') | $(result_line json_dumps_py 'bytes:') |"
if [ "$have_leibniz" = 1 ]; then
	emit "| leibniz | $(result_line leibniz_lf 'pi:') | pi = $leibniz_py_result |"
	emit "| leibniz-matrix | $(result_line leibniz_matrix_lf 'pi:') | pi = $leibniz_py_result |"
fi
emit ""

log "done."
