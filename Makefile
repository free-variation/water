CC     = clang
CFLAGS = -O3 -march=native -Wall -Wextra
LDLIBS = -lm

SRCS = src/c/core.c src/c/words.c src/c/collections.c src/c/matrix.c src/c/functional.c src/c/superwords.c src/c/strings.c src/c/help_table.c src/c/logic.c
HDRS = src/c/logicforth.h src/c/lib_embed.h

# Vendored PCRE2 (see external/pcre2/PROVENANCE; refresh with tools/vendor-pcre2.sh).
PCRE2_DIR    = external/pcre2
PCRE2_SRC    = $(PCRE2_DIR)/src
PCRE2_LIB    = $(PCRE2_DIR)/libpcre2-8.a
PCRE2_CFLAGS = -O2 -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC -I$(PCRE2_SRC)
PCRE2_OBJS   = $(patsubst %.c,%.o,$(wildcard $(PCRE2_SRC)/pcre2_*.c))

logicforth: $(SRCS) $(HDRS) $(PCRE2_LIB)
	$(CC) $(CFLAGS) -I$(PCRE2_SRC) -o logicforth $(SRCS) $(PCRE2_LIB) $(LDLIBS)

$(PCRE2_LIB): $(PCRE2_OBJS)
	ar rcs $@ $(PCRE2_OBJS)

$(PCRE2_SRC)/%.o: $(PCRE2_SRC)/%.c
	$(CC) $(PCRE2_CFLAGS) -c $< -o $@

src/c/help_table.c: docs/reference.md tools/gen-help.py
	python3 tools/gen-help.py

src/c/lib_embed.h: src/forth/lib.l4
	cd src/forth && xxd -i lib.l4 > ../../src/c/lib_embed.h

vendor-pcre2:
	sh tools/vendor-pcre2.sh

test: logicforth
	sh tests/run.sh

bench:
	@sh bench/run-benchmarks.sh

clean:
	rm -f logicforth $(PCRE2_OBJS) $(PCRE2_LIB)

.PHONY: clean test bench vendor-pcre2
