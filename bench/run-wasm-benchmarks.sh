#!/usr/bin/env bash
#
# Run the wasm-eligible water benchmarks three ways — CPython, native
# water, and the wasm/WASI build under a WASI runtime — and emit a markdown
# comparison on stdout. All progress goes to stderr:
#
#   bench/run-wasm-benchmarks.sh > bench/wasm-$(date +%Y%m%d).md
#
# The set is the pure compute / regex / json pyperformance ports; the process,
# FFI/LAPACK, and sqlite benchmarks are excluded because those subsystems are
# unavailable on WASI (they error as stubs). Problem sizes match the native
# harness for comparability, so wasm times are slower in wall-clock.
#
# Environment overrides:
#   PYTHON        python interpreter          (default: python3.14)
#   WASMTIME      WASI runtime on PATH or path (default: wasmtime)
#   REPS          native/wasm reps per bench   (default: 3)
#   REPS_PY       python reps per bench        (default: 2)
#   RUN_LEIBNIZ   set to 1 to include leibniz  (very slow on wasm)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
bin="$root/water"
wasm="$root/water.wasm"

python=${PYTHON:-python3.14}
if [ -n "${CRYPTO_PYTHON:-}" ]; then
	crypto_python=$CRYPTO_PYTHON
elif [ -x "$root/.venv/bin/python" ]; then
	crypto_python="$root/.venv/bin/python"
else
	crypto_python=$python
fi
wasmtime=${WASMTIME:-wasmtime}
reps=${REPS:-3}
reps_py=${REPS_PY:-2}
reps_compile=${REPS_COMPILE:-15}
run_leibniz=${RUN_LEIBNIZ:-0}

# Problem sizes default to the native harness's values (for comparability) but
# are env-overridable, so a quick wasm pass can shrink the heavy compute loops —
# the wasm/native and py/wasm ratios hold at smaller sizes.
nbody_steps=${NBODY_STEPS:-20000}
raytrace_loops=${RAYTRACE_LOOPS:-10}
float_points=${FLOAT_POINTS:-100000}
float_repeat=${FLOAT_REPEAT:-20}
crypto_loops=${CRYPTO_LOOPS:-10}
spectral_loops=${SPECTRAL_LOOPS:-50}
scimark_lu_cycles=${SCIMARK_LU_CYCLES:-100}
nqueens_n=${NQUEENS_N:-8}
fannkuch_n=${FANNKUCH_N:-9}
scimark_sor_loops=${SCIMARK_SOR_LOOPS:-100}
scimark_sparse_cycles=${SCIMARK_SPARSE_CYCLES:-500}
fft_loops=${FFT_LOOPS:-5}
fft_cycles=${FFT_CYCLES:-50}
barnes_iterations=${BARNES_ITERATIONS:-50}
barnes_loops=${BARNES_LOOPS:-2}
montecarlo_samples=${MONTECARLO_SAMPLES:-1000000}
montecarlo_loops=${MONTECARLO_LOOPS:-3}
meteor_loops=${METEOR_LOOPS:-10}
hexiom_loops=${HEXIOM_LOOPS:-50}
leibniz_rounds=${LEIBNIZ_ROUNDS:-1000000000}

work=$(mktemp -d "${TMPDIR:-/tmp}/lfwasmbench.XXXXXX")
trap 'rm -rf "$work"' EXIT

log() { printf '%s\n' "$*" >&2; }

# --- build -----------------------------------------------------------------
log "building native + wasm..."
(cd "$root" && make water) >&2 || { log "native build failed"; exit 1; }
(cd "$root" && make wasm) >&2 || { log "wasm build failed"; exit 1; }
if ! command -v "$wasmtime" >/dev/null 2>&1 && [ ! -x "$wasmtime" ]; then
	log "wasmtime not found (set WASMTIME to a WASI runtime on PATH or a path)"; exit 1
fi

# --- helpers ---------------------------------------------------------------
median() {
	sort -n | awk '{ a[NR] = $1 }
		END {
			if (NR == 0) { print "nan"; exit }
			if (NR % 2) { print a[(NR + 1) / 2] }
			else { print (a[NR / 2] + a[NR / 2 + 1]) / 2 }
		}'
}
fmt_s() { awk -v v="$1" 'BEGIN { if (v == "" || v == "nan") { print "—"; exit } printf (v >= 1 ? "%.3f s" : "%.4f s"), v }'; }
ratio() {
	awk -v a="$1" -v b="$2" 'BEGIN {
		if (a == "" || b == "" || a == "nan" || b == "nan" || b == 0) { print "—"; exit }
		r = a / b
		if (r >= 10) printf "~%.0f×", r; else printf "%.2f×", r
	}'
}

# --- bench input feeds (stdin for native and wasm) -------------------------
feed_nqueens()  { cat "$here/pyperformance/nqueens.h2o"; }
feed_fannkuch() { cat "$here/pyperformance/fannkuch.h2o"; }
feed_nbody()    { cat "$here/pyperformance/nbody.h2o"; }
feed_raytrace() { cat "$here/pyperformance/raytrace.h2o"; }
feed_float()    { cat "$here/pyperformance/float.h2o"; }
feed_crypto()   { cat "$here/pyperformance/crypto-pyaes.h2o"; }
feed_spectral() { cat "$here/pyperformance/spectral-norm.h2o"; }
feed_scimark_lu() { cat "$here/pyperformance/scimark-lu.h2o"; }
feed_scimark_sor() { cat "$here/pyperformance/scimark-sor.h2o"; }
feed_scimark_sparse() { cat "$here/pyperformance/scimark-sparse.h2o"; }
feed_scimark_fft() { cat "$here/pyperformance/scimark-fft.h2o"; }
feed_barnes()   { cat "$here/pyperformance/barnes-hut.h2o"; }
feed_scimark_mc() { cat "$here/pyperformance/scimark-montecarlo.h2o"; }
feed_meteor()   { cat "$here/pyperformance/meteor.h2o"; }
feed_hexiom()   { cat "$here/pyperformance/hexiom.h2o"; }
feed_regex_dna() { cat "$here/pyperformance/regex-dna.h2o"; }
feed_regex_compile() { cat "$here/pyperformance/regex-compile.h2o"; }
feed_regex_effbot() { cat "$here/pyperformance/regex-effbot.h2o"; }
feed_regex_v8() { cat "$here/pyperformance/regex-v8.h2o"; }
feed_deepcopy() { cat "$here/pyperformance/deepcopy.h2o"; }
feed_json_loads() { cat "$here/pyperformance/json-loads.h2o"; }
feed_json_dumps() { cat "$here/pyperformance/json-dumps.h2o"; }
feed_leibniz()  { cat "$here/pyperformance/leibniz.h2o"; }
feed_logic()    { cat "$here/logic/logic.h2o"; }
feed_nreverse() { cat "$here/logic/nreverse.h2o"; }

# --- python wrappers -------------------------------------------------------
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
py_none()       { :; }

# --- runners ---------------------------------------------------------------
# native: feed | bin ; wasm: feed | wasmtime run wasm ; python: py wrapper
run_native() { local key=$1 feed=$2 n=$3 i; : > "$work/$key.nat"; for i in $(seq 1 "$n"); do "$feed" | "$bin" >> "$work/$key.nat" 2>/dev/null; done; }
run_wasm()   { local key=$1 feed=$2 n=$3 i; : > "$work/$key.wasm"; for i in $(seq 1 "$n"); do "$feed" | "$wasmtime" run --dir "$root" "$wasm" >> "$work/$key.wasm" 2>/dev/null; done; }
run_pyth()   { local key=$1 pyfn=$2 n=$3 i; : > "$work/$key.py"; for i in $(seq 1 "$n"); do "$pyfn" >> "$work/$key.py" 2>/dev/null; done; }

med() { grep -i elapsed "$work/$1" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | median; }

# one benchmark, all three runtimes; rows appended to the table buffer
declare -a ROWS
bench() {
	# $1 label  $2 size  $3 feed_fn  $4 py_fn  $5 reps_native/wasm  $6 reps_py
	local label=$1 size=$2 feed=$3 pyfn=$4 rn=${5:-$reps} rp=${6:-$reps_py}
	log "== $label =="
	run_pyth   "$label" "$pyfn" "$rp"
	run_native "$label" "$feed" "$rn"
	run_wasm   "$label" "$feed" "$rn"
	local p n w
	p=$(med "$label.py"); n=$(med "$label.nat"); w=$(med "$label.wasm")
	ROWS+=("| $label | $size | $(fmt_s "$p") | $(fmt_s "$n") | $(fmt_s "$w") | $(ratio "$w" "$n") | $(ratio "$w" "$p") |")
}

# --- run everything --------------------------------------------------------
log "native/wasm: $reps reps   python: $reps_py reps   ($python; $wasmtime)"

bench nqueens        "N = $nqueens_n"                    feed_nqueens        py_nqueens
bench nbody          "${nbody_steps} steps"              feed_nbody          py_nbody
bench raytrace       "${raytrace_loops}× 100×100"        feed_raytrace       py_raytrace
bench float          "${float_points} pts × ${float_repeat}" feed_float      py_float
bench crypto-pyaes   "8192 B, ${crypto_loops}×"          feed_crypto         py_crypto
bench fannkuch       "N = $fannkuch_n"                   feed_fannkuch       py_fannkuch
bench spectral-norm  "N = 130, ${spectral_loops}×"       feed_spectral       py_spectral
bench scimark-lu     "N=100, ${scimark_lu_cycles}×"      feed_scimark_lu     py_scimark_lu
bench scimark-sparse "N=1000, ${scimark_sparse_cycles}×" feed_scimark_sparse py_scimark_sparse
bench scimark-fft    "N=1024, ${fft_loops}×${fft_cycles}" feed_scimark_fft   py_scimark_fft
bench barnes-hut     "200 bodies, ${barnes_loops}×${barnes_iterations}" feed_barnes py_barnes
bench scimark-sor    "N=100, 10×${scimark_sor_loops}"    feed_scimark_sor    py_scimark_sor
bench scimark-montecarlo "${montecarlo_samples}×${montecarlo_loops}" feed_scimark_mc py_scimark_mc
bench meteor         "${meteor_loops} solves"            feed_meteor         py_meteor
bench hexiom         "level 25, ${hexiom_loops}"         feed_hexiom         py_hexiom
bench regex-dna      "100K → 1M"                         feed_regex_dna      py_regex_dna
bench regex-compile  "239 patterns, cold"               feed_regex_compile  py_regex_compile "$reps_compile" "$reps_compile"
bench regex-effbot   "21 pat × 0..10k"                   feed_regex_effbot   py_regex_effbot
bench regex-v8       "12 blocks"                         feed_regex_v8       py_regex_v8
bench deepcopy       "N=20000, 60/N"                     feed_deepcopy       py_deepcopy
bench json-loads     "222k parses"                       feed_json_loads     py_json_loads
bench json-dumps     "EMPTY/…/HUGE ×250"                 feed_json_dumps     py_json_dumps
[ "$run_leibniz" = 1 ] && bench leibniz "${leibniz_rounds} it" feed_leibniz py_nqueens 1 1

bench logic          "deep/wide/frame unify"             feed_logic          py_none 1 1
bench nreverse       "nrev(30) × 30k"                    feed_nreverse       py_none 2 1

# --- emit report -----------------------------------------------------------
pyver=$("$python" --version 2>&1 | awk '{print $2}')
wtver=$("$wasmtime" --version 2>&1 | awk '{print $2}')
emit() { printf '%s\n' "$*"; }

emit "# WASM benchmark report"
emit ""
emit "Generated by \`bench/run-wasm-benchmarks.sh\` ($reps native/wasm reps, $reps_py python reps; medians)."
emit ""
emit "- **Host**: $(uname -sr)"
emit "- **Native**: \`clang -O3 -march=native\`"
emit "- **wasm**: \`wasm32-wasi\`, wasi-sdk; run under wasmtime $wtver (1 GB arena)"
emit "- **Python**: CPython $pyver (\`$python\`)"
emit "- **Date**: $(date +%Y-%m-%d)"
emit ""
emit "\`wasm/native\` and \`wasm/py\` are wasm runtime relative to native and CPython (>1 = wasm slower)."
emit ""
emit "| benchmark | size | python | native | wasm | wasm/native | wasm/py |"
emit "|:----------|:-----|-------:|-------:|-----:|------------:|--------:|"
for r in "${ROWS[@]}"; do emit "$r"; done
emit ""

log "done."
