CC     = clang
CFLAGS = -O3 -march=native -Wall -Wextra

SRCS = src/c/core.c src/c/words.c src/c/collections.c src/c/matrix.c
HDRS = src/c/logicforth.h

logicforth: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o logicforth $(SRCS)

test: logicforth
	tests/run.sh

bench/bench_dgemm: bench/bench_dgemm.c
	$(CC) $(CFLAGS) -DACCELERATE_NEW_LAPACK -framework Accelerate \
		-o bench/bench_dgemm bench/bench_dgemm.c

bench: bench/bench_dgemm
	bench/bench_dgemm

clean:
	rm -f logicforth bench/bench_dgemm

.PHONY: clean test bench
