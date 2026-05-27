CC     = clang
CFLAGS = -O3 -march=native -Wall -Wextra
LDLIBS = -lm

SRCS = src/c/core.c src/c/words.c src/c/collections.c src/c/matrix.c
HDRS = src/c/logicforth.h

logicforth: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o logicforth $(SRCS) $(LDLIBS)

test: logicforth
	sh tests/run.sh

clean:
	rm -f logicforth

.PHONY: clean test
