CC     = clang
CFLAGS = -O3 -march=native -Wall -Wextra -pthread
LDLIBS = -lm

SRCS = src/c/core.c src/c/words.c src/c/collections.c src/c/matrix.c src/c/functional.c src/c/superwords.c src/c/strings.c src/c/help_table.c src/c/logic.c src/c/database.c
HDRS = src/c/logicforth.h src/c/lib_embed.h

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
SQLITE_CFLAGS = -O2 -DSQLITE_THREADSAFE=2 -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
                -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
                -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
                -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA
SQLITE_OBJ    = $(SQLITE_DIR)/sqlite3.o

# Vendored isocline (see external/isocline/PROVENANCE; refresh with tools/vendor-isocline.sh).
# Compiles as a single source unit (src/isocline.c) per its readme.md.
ISOCLINE_DIR    = external/isocline
ISOCLINE_CFLAGS = -O2 -I$(ISOCLINE_DIR)/include
ISOCLINE_OBJ    = $(ISOCLINE_DIR)/isocline.o

logicforth: $(SRCS) $(HDRS) $(PCRE2_LIB) $(SQLITE_OBJ) $(ISOCLINE_OBJ)
	$(CC) $(CFLAGS) -I$(PCRE2_SRC) -I$(SQLITE_DIR) -I$(ISOCLINE_DIR)/include -o logicforth $(SRCS) $(PCRE2_LIB) $(SQLITE_OBJ) $(ISOCLINE_OBJ) $(LDLIBS)

$(PCRE2_LIB): $(PCRE2_OBJS)
	ar rcs $@ $(PCRE2_OBJS)

$(PCRE2_SRC)/%.o: $(PCRE2_SRC)/%.c
	$(CC) $(PCRE2_CFLAGS) -c $< -o $@

$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	$(CC) $(SQLITE_CFLAGS) -c $< -o $@

$(ISOCLINE_OBJ): $(ISOCLINE_DIR)/src/isocline.c
	$(CC) $(ISOCLINE_CFLAGS) -c $< -o $@

src/c/help_table.c: docs/reference.md tools/gen-help.py
	python3 tools/gen-help.py

src/c/lib_embed.h: src/forth/lib.l4
	cd src/forth && xxd -i lib.l4 > ../../src/c/lib_embed.h

# Regenerate the editor syntax files from docs/reference.md (not compiled, so
# on-demand rather than a build dependency). Run after editing reference.md.
editors: docs/reference.md tools/gen-editors.py
	python3 tools/gen-editors.py

vendor-pcre2:
	sh tools/vendor-pcre2.sh

test: logicforth
	sh tests/run.sh

bench:
	@sh bench/run-benchmarks.sh

clean:
	rm -f logicforth $(PCRE2_OBJS) $(PCRE2_LIB) $(SQLITE_OBJ) $(ISOCLINE_OBJ)

.PHONY: clean test bench vendor-pcre2 editors
