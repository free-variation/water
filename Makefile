CC     = clang
CFLAGS = -O3 -march=native -Wall -Wextra
LDLIBS = -lm

SRCS = src/c/core.c src/c/words.c src/c/collections.c src/c/matrix.c src/c/functional.c src/c/superwords.c src/c/strings.c src/c/help_table.c src/c/logic.c
HDRS = src/c/logicforth.h

PCRE2 = /opt/homebrew/opt/pcre2

logicforth: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -I$(PCRE2)/include -o logicforth $(SRCS) $(PCRE2)/lib/libpcre2-8.a $(LDLIBS)

src/c/help_table.c: docs/reference.md tools/gen-help.py
	python3 tools/gen-help.py

test: logicforth
	sh tests/run.sh

bench:
	@sh bench/run-benchmarks.sh

clean:
	rm -f logicforth

.PHONY: clean test bench pcre2
