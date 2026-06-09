# scratch — small utilities, in build order

Working checklist pulled from PLAN.md (the quick, self-contained tier).
Big subsystems (SQLite, HTTP, FFI, logic, threads, BLAS, Unicode codec,
PCRE2 vendoring) are not here.

## 1. File I/O — whole-file, no handles (~15 lines C each)  ✓ DONE (tests/111)
- [x] `read-file ( path -- string )` — read whole file; error if missing
- [x] `write-file ( string path -- )` — create/truncate, write
- [x] `append-file ( string path -- )` — open append, write, close

## 2. Random numbers — PRNG ~30 lines
- [ ] `random-float ( -- f )` — uniform [0, 1)
- [ ] `min max random-int ( -- n )`
- [ ] `seed seed! ( -- )`
- [ ] `array shuffle ( -- arr )` — Fisher-Yates
- [ ] `array sample ( -- elt )` (lib, over random-int)

## 3. Sort — ~30 lines
- [ ] `array sort` — by val_cmp, input untouched
- [ ] `array [: x y -- cmp :] sort-with`

## 4. Time / dates — `now` already built
- [ ] `"%Y-%m-%d %H:%M:%S" time-format` — strftime, UTC default
- [ ] `"2026-05-25" time-parse` — strptime

## 5. Standard streams — `env` / `env!` done
- [ ] `stdin` / `stdout` / `stderr` as T_STREAM (fds 0/1/2);
      read/write via the subprocess stream words

## 6. Error handling — ~10 lines
- [ ] `catch` / `try-catch` intercept interpreter `error_flag`, not just `throw`

## 7. Format specifiers
- [ ] extend `format`: `{0:.2f}`, `{0:8}`, `{0:x}`

## 8. Path keys in frame literals
- [ ] `{ /addr/city C }` → `{ :addr { :city C } }` (reuse `!` vivify walk)

## 9. String wrappers — lib over the regex layer
- [ ] `index-of`
- [ ] `starts-with`
- [ ] `ends-with`
- [ ] `trim`
- [ ] `lines`

## 10. Functional primitives — ~60 lines lib
- [ ] `find` (short-circuit via shift)
- [ ] `any?`
- [ ] `all?`
- [ ] `flat-map`
- [ ] `sort-by`
- [ ] `each`
- [ ] `group-by` (→ frame)
- [ ] `partition`

## 11. Help system  ✓ DONE (tests/112)
- [x] `help ( xt -- fr )` — frame of a word's reference entry; generated from
      docs/reference.md by tools/gen-help.py into src/c/help_table.c

## 12. Matrix — small, deferred-until-use
- [ ] `argmax` / `argmin` — index (or `(i,j)`) of max/min element
