# Testing

Run everything: `make test`. Run one program: `make tests/test_job && ./tests/test_job`.
Every check prints `[PASS]` or `[FAIL]` with the expected and the actual value; each program ends
with a summary and exits with 0 only if all checks passed. The applications also check their own
answer against a plain one-pass computation.

| Program | Checks | What it demonstrates | Case types |
|---|---|---|---|
| `test_kv` | 38 | pairs are copied; list grows 8 -> 16 -> ...; empty key/value, keys with `\0`; sorting; freeing twice | normal, boundary, invalid, memory |
| `test_split` | 39 | byte / word / line modes: splits cover the input exactly once, in order; no word or line is cut; empty input; split size 0; 20 MB input | normal, boundary, invalid, empty, large |
| `test_partition` | 11 | the hash is real 64-bit FNV-1a (reference values); same key -> same partition; index always in range; keys are spread | normal, boundary |
| `test_job` | 39 | the whole library with answers checkable by hand: word count; **same answer for any number of workers and any split size**; defaults; running a job twice; `ctx` reaches map; reduce emitting 0 or many pairs; values sorted; file input/output; sorted output | normal, boundary, empty |
| `test_threads` | 19 | same answer for all 1..8 mappers x 1..8 reducers; thread counts used; **mappers and reducers really run at the same time** (each call sleeps 30 ms or 20 ms: with 1 mapper 8 splits take about 8 x 30 ms, with 4 mappers the same job must be at least 2x faster; sleeping works on any number of cores); the 64-worker limit; 300 stress runs | threads |
| `test_errors` | 46 | NULL in every function, invalid values, wrong call order, map/reduce function that fails, invalid pairs, missing/unreadable files: always an error code, never a crash | invalid, failure |
| `test_memory` | 5 | 200 complete jobs, 100 failing jobs, reruns, replacing file input, destroying a job in every state | memory |
| `test_large` | 13 | 2,000,000 words on 4+4 threads; 500,000 different keys; 100,000 splits; a 100 KB key and a 1 MB value | large |
| `test_alloc_fail` | 8 | makes the n-th `malloc`/`calloc`/`realloc` fail, for every n until the job succeeds: it must return `MR_ERR_NO_MEMORY` and never crash | failure, memory |

Total: 218 checks in 9 programs, plus the five applications (word count, inverted index, word
frequency, maximum per key, matrix multiplication with several sizes and worker counts) which are
self-checking and run by `make test`. The separate simulation has its own 179 checks (`make sim-test`).

## Tools

| Command | What it finds |
|---|---|
| `make asan` | memory errors (invalid reads/writes, leaks) and undefined behaviour, in every test and application |
| `make tsan` | data races between threads (ThreadSanitizer) in the job/thread/error/memory tests and two applications |
| `make valgrind` | leaks and invalid accesses (needs `valgrind` installed) |

Results of the last run on the development machine: all tests pass; AddressSanitizer/UBSan: no
errors; ThreadSanitizer: no races; Valgrind: no errors, no leaks.

## Limits of this testing (be honest about them)

* The thread tests show real concurrency and absence of races in the runs that were made; a race
  detector cannot prove that no race exists in every possible schedule.
* Speed-up numbers depend on the machine (see `ANALYSIS.md`); there is no test that requires a
  specific speed-up.
* The inputs are generated or small; no real-world corpus was used.
