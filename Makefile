CC     = clang
CFLAGS = -O3 -march=native -Wall -Wextra

logicforth: src/c/logicforth.c
	$(CC) $(CFLAGS) -o logicforth src/c/logicforth.c

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
