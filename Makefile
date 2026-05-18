CC     = cc
CFLAGS = -O2 -Wall -Wextra

logicforth: src/c/logicforth.c
	$(CC) $(CFLAGS) -o logicforth src/c/logicforth.c

test: logicforth
	tests/run.sh

clean:
	rm -f logicforth

.PHONY: clean test
