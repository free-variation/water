CC     = clang
CFLAGS = -O3 -march=native -Wall -Wextra -pthread
LDLIBS = -lm -lffi

SRCS = src/c/core.c src/c/words.c src/c/compiler.c src/c/io.c src/c/image.c src/c/collections.c src/c/matrix.c src/c/functional.c src/c/superwords.c src/c/strings.c src/c/help_table.c src/c/logic.c src/c/database.c src/c/foreign.c src/c/platform_posix.c src/c/dimension.c src/c/time.c
HDRS = src/c/water.h src/c/platform.h src/c/lib_embed.h src/c/logo_embed.h src/c/repl_highlight_groups.h

# Vendored PCRE2 (see external/pcre2/PROVENANCE; refresh with tools/vendor-pcre2.sh).
PCRE2_DIR    = external/pcre2
PCRE2_SRC    = $(PCRE2_DIR)/src
PCRE2_LIB    = $(PCRE2_DIR)/libpcre2-8.a
PCRE2_CFLAGS = -O2 -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC -I$(PCRE2_SRC)
PCRE2_OBJS   = $(patsubst %.c,%.o,$(wildcard $(PCRE2_SRC)/pcre2_*.c))

# Vendored SQLite (see external/sqlite/PROVENANCE; refresh with tools/vendor-sqlite.sh).
# Compiled once to a cached object and linked into the binary. THREADSAFE=2
# (not 0): each thread uses its own connection per PLAN.md's HTTP worker pool and
# fork-join models; 0 would drop SQLite's internal-global mutexing and corrupt them.
SQLITE_DIR    = external/sqlite
SQLITE_DEFS   = -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
                -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
                -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
                -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA
SQLITE_CFLAGS = -O2 -DSQLITE_THREADSAFE=2 $(SQLITE_DEFS)
SQLITE_OBJ    = $(SQLITE_DIR)/sqlite3.o

# Vendored isocline (see external/isocline/PROVENANCE; refresh with tools/vendor-isocline.sh).
# Compiles as a single source unit (src/isocline.c) per its readme.md.
ISOCLINE_DIR    = external/isocline
ISOCLINE_CFLAGS = -O2 -I$(ISOCLINE_DIR)/include
ISOCLINE_OBJ    = $(ISOCLINE_DIR)/isocline.o

# Vendored LAPACKE closure (see external/lapacke/PROVENANCE; refresh with
# tools/vendor-lapacke.sh). C wrappers over Accelerate's Fortran LAPACK, built
# into a dylib that water dlopens via FFI. macOS/Accelerate-only, so this
# is the explicit `make lapacke` target, not part of the default build.
# Add a routine: re-vendor with it, then add a -exported_symbol below.
LAPACKE_DIR     = external/lapacke
LAPACKE_SRCS    = $(wildcard $(LAPACKE_DIR)/src/*.c) $(wildcard $(LAPACKE_DIR)/utils/*.c)
LAPACKE_OBJS    = $(patsubst %.c,%.o,$(LAPACKE_SRCS))
LAPACKE_LIB     = $(LAPACKE_DIR)/liblapacke.a
LAPACKE_DYLIB   = $(LAPACKE_DIR)/liblapacke_accel.dylib
LAPACKE_CFLAGS  = -O2 -DNDEBUG -DADD_ -I$(LAPACKE_DIR)/include
LAPACKE_EXPORTS = -Wl,-exported_symbol,_LAPACKE_dgesvd \
                  -Wl,-exported_symbol,_LAPACKE_dgelsd

all: water $(LAPACKE_DYLIB)

water: $(SRCS) $(HDRS) $(PCRE2_LIB) $(SQLITE_OBJ) $(ISOCLINE_OBJ)
	$(CC) $(CFLAGS) -I$(PCRE2_SRC) -I$(SQLITE_DIR) -I$(ISOCLINE_DIR)/include -o water $(SRCS) $(PCRE2_LIB) $(SQLITE_OBJ) $(ISOCLINE_OBJ) $(LDLIBS)

$(PCRE2_LIB): $(PCRE2_OBJS)
	ar rcs $@ $(PCRE2_OBJS)

$(PCRE2_SRC)/%.o: $(PCRE2_SRC)/%.c
	$(CC) $(PCRE2_CFLAGS) -c $< -o $@

$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	$(CC) $(SQLITE_CFLAGS) -c $< -o $@

$(ISOCLINE_OBJ): $(ISOCLINE_DIR)/src/isocline.c
	$(CC) $(ISOCLINE_CFLAGS) -c $< -o $@

# WASM/WASI cross-build (a-shell, standalone runtimes, browser via a WASI shim).
# Shares the vendored pcre2 dependency, compiled no-JIT for wasm, and sqlite
# (compiled single-threaded, THREADSAFE=0). isocline and ffi are unavailable on
# WASI and excluded, their words present as erroring stubs from platform_wasi.c.
# Needs the wasi-sdk toolchain in $(WASI_SDK).
WASI_SDK        = $(HOME)/wasi-sdk
WASI_CC         = $(WASI_SDK)/bin/clang
WASI_AR         = $(WASI_SDK)/bin/llvm-ar
WASI_SYSROOT    = $(WASI_SDK)/share/wasi-sysroot
WASM_SRCS       = src/c/core.c src/c/words.c src/c/compiler.c src/c/io.c src/c/image.c src/c/collections.c src/c/matrix.c src/c/functional.c src/c/superwords.c src/c/strings.c src/c/help_table.c src/c/logic.c src/c/database.c src/c/dimension.c src/c/platform_wasi.c src/c/time.c
WASM_CFLAGS     = --sysroot $(WASI_SYSROOT) -O2 -I$(PCRE2_SRC) -I$(SQLITE_DIR) -Wno-ignored-pragmas -Wl,-z,stack-size=8388608
WASM_PCRE2_OBJS = $(patsubst %.c,%.wasm.o,$(wildcard $(PCRE2_SRC)/pcre2_*.c))
WASM_PCRE2_LIB  = $(PCRE2_DIR)/libpcre2-8-wasm.a
WASM_SQLITE_OBJ = $(SQLITE_DIR)/sqlite3.wasm.o

wasm: water.wasm

water.wasm: $(WASM_SRCS) $(HDRS) $(WASM_PCRE2_LIB) $(WASM_SQLITE_OBJ)
	$(WASI_CC) $(WASM_CFLAGS) -o water.wasm $(WASM_SRCS) $(WASM_PCRE2_LIB) $(WASM_SQLITE_OBJ)

$(WASM_PCRE2_LIB): $(WASM_PCRE2_OBJS)
	$(WASI_AR) rcs $@ $(WASM_PCRE2_OBJS)

$(PCRE2_SRC)/%.wasm.o: $(PCRE2_SRC)/%.c
	$(WASI_CC) --sysroot $(WASI_SYSROOT) $(PCRE2_CFLAGS) -c $< -o $@

$(WASM_SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	$(WASI_CC) --sysroot $(WASI_SYSROOT) -O2 -DSQLITE_THREADSAFE=0 $(SQLITE_DEFS) -c $< -o $@

# Build the LAPACKE-over-Accelerate dylib that FFI dlopens.
lapacke: $(LAPACKE_DYLIB)

# -exported_symbol roots the routines we expose (the linker pulls their closure
# from the archive) and limits the dylib's exports to them; -dead_strip drops
# anything unreachable. -framework Accelerate resolves the Fortran dgesvd_/etc.
$(LAPACKE_DYLIB): $(LAPACKE_LIB)
	$(CC) -dynamiclib -o $@ $(LAPACKE_EXPORTS) -Wl,-dead_strip $(LAPACKE_LIB) -framework Accelerate

$(LAPACKE_LIB): $(LAPACKE_OBJS)
	ar rcs $@ $(LAPACKE_OBJS)

$(LAPACKE_DIR)/%.o: $(LAPACKE_DIR)/%.c
	$(CC) $(LAPACKE_CFLAGS) -c $< -o $@

src/c/help_table.c: docs/reference.md tools/gen-help.py
	python3 tools/gen-help.py

src/c/lib_embed.h: src/forth/lib.h2o
	cd src/forth && xxd -i lib.h2o > ../../src/c/lib_embed.h

src/c/logo_embed.h: water-logo.txt
	xxd -i water-logo.txt > src/c/logo_embed.h

# Regenerate the editor syntax files from docs/reference.md (not compiled, so
# on-demand rather than a build dependency). Run after editing reference.md.
editors src/c/repl_highlight_groups.h: docs/reference.md tools/gen-editors.py
	python3 tools/gen-editors.py

vendor-pcre2:
	sh tools/vendor-pcre2.sh

vendor-lapacke:
	sh tools/vendor-lapacke.sh

test: water
	sh tests/run.sh

# Runs the golden suite against the wasm build under a WASI runtime. The runner
# finds wasmtime on PATH or ~/.wasmtime/bin; otherwise set WASMTIME=<path>.
test-wasm: water.wasm
	sh tests/run-wasm.sh

bench:
	@sh bench/run-benchmarks.sh

clean:
	rm -f water water.wasm $(PCRE2_OBJS) $(PCRE2_LIB) $(WASM_PCRE2_OBJS) $(WASM_PCRE2_LIB) $(SQLITE_OBJ) $(WASM_SQLITE_OBJ) $(ISOCLINE_OBJ) $(LAPACKE_OBJS) $(LAPACKE_LIB) $(LAPACKE_DYLIB)

.PHONY: all clean test test-wasm bench wasm vendor-pcre2 vendor-lapacke lapacke editors
