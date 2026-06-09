#!/bin/sh
# Interactive logicforth REPL with line editing, history, and tab completion
# (Forth words from forth-words.txt, plus filenames) via rlwrap.
# Falls back to the bare binary if rlwrap isn't installed.

here=$(cd "$(dirname "$0")" && pwd)
binary="$here/logicforth"
wordlist="$here/forth-words.txt"
history="$here/.logicforth_history"

if command -v rlwrap >/dev/null 2>&1; then
    exec rlwrap -c -f "$wordlist" -H "$history" "$binary"
else
    exec "$binary"
fi
