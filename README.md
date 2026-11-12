# MapReduce Library in C (multithreaded)

A reusable C library for the MapReduce programming model. **You write two functions, `map()` and
`reduce()`; the library splits the input, runs **one mapper (thread) per split**, partitions and shuffles the
intermediate pairs, groups them by key, runs reducer threads and gives you the final output.**

Team: _<team name / members>_ · GitHub: _<repository link>_

> **What this is, honestly:** a *shared-memory* MapReduce (like Stanford's Phoenix), where
> workers are POSIX threads in one process. It follows the Hadoop model (map, partition, shuffle,
> sort/group by key, reduce) and the same programming style, but it does **not** spread work over
> several computers and has no distributed file system. See "Limitations".

The folder also contains **`simulation/`**, the earlier midterm program that *simulates* the first
half of the pipeline step by step (no threads). It is a separate project with its own header,
Makefile and tests, kept for learning and demonstrating the workflow. It does **not** use or
depend on the library.

## Contents

```
include/mapreduce.h      the one public header (API, with comments)
src/                     the library (9 files, see below)
examples/                the five applications + the worker-count benchmark
tests/                   9 test programs
documentation/           DESIGN.md, API.md, TESTING.md, ANALYSIS.md
results/                 saved output of the worker analysis
simulation/              the separate step-by-step simulation (own README, Makefile, tests)
Makefile                 builds and tests everything
```

### The library files (`src/`) and how they connect

| File | Purpose |
|---|---|
| `mr_error.c` | error code -> text (`mr_error_string`) |
| `mr_kv.c` | key-value pair, growing list of pairs, comparing and sorting pairs |
| `mr_split.c` | cuts the input into splits (byte, word or line boundaries) |
| `mr_partition.c` | FNV-1a hash: which partition (reducer) owns a key |
| `mr_pool.c` | **thread management**: runs tasks on N POSIX threads with a mutex-protected task counter |
| `mr_map.c` | `mr_emit` and the **map phase** (one task per split) |
| `mr_shuffle.c` | the **shuffle phase** (gather one partition's pairs, sort by key; one task per partition) |
| `mr_reduce.c` | grouping and the **reduce phase** (one task per partition), builds the final output |
| `mr_job.c` | the job object, settings, `mr_job_run` (runs the four phases), statistics, file input/output |
| `mr_internal.h` | private declarations shared by these files (users never include it) |

`mr_job.c` is the conductor: it calls split -> map -> shuffle -> reduce, and each of the three
parallel phases uses `mr_pool.c`. Everything is linked into one archive, `libmr.a`.

## Build and run

Requirements: gcc (or clang), make, a POSIX system (Linux/macOS/WSL). Valgrind is optional.

```
make                      # builds libmr.a, the 5 applications, the benchmark and all tests
make test                 # runs every test program and every application (self-checking)
```

### Number of mappers (Hadoop style)

The number of mappers is **not set by the user**: it is the number of splits, i.e. input size /
split size (rounded up), and every split is its own mapper. At most 64 mapper threads (`MR_MAX_WORKERS`)
run at the same time; when there are more splits, a thread that finishes takes the next split.
The optional `[max-mappers]` argument of the examples (and `mr_job_set_mappers`) only lowers
that limit of simultaneous mapper threads; it does not change the number of mappers. The number of
reducers is still chosen by the user (`mr_job_set_reducers`).

### The five applications (all use the library's `map`/`reduce` API)

Each prints its result and checks it against an independent one-pass computation, and exits
with 1 if the answer is wrong. With no arguments they use a small built-in input.

| Application | Command | What it shows |
|---|---|---|
| Word count | `./examples/word_count [file\|-] [max-mappers] [reducers]` | map emits (word,1); reduce adds |
| Inverted index | `./examples/inverted_index [file\|-] [max-mappers] [reducers] [query words]` | which documents contain each word (one document per line: `doc1 the cat ...`) |
| Word frequency analysis | `./examples/word_frequency [file\|-] [max-mappers] [reducers]` | **two chained jobs**: word count, then "how many words occur n times"; prints top words with their share |
| Maximum value per key | `./examples/max_per_key [file\|-] [max-mappers] [reducers]` | input lines `city 31.5`; reduce keeps the largest |
| Matrix multiplication | `./examples/matrix_multiply [n m p] [max-mappers] [reducers]` | C = A x B in one job; `ctx` carries the sizes; checked against a plain triple loop |

Examples:

```
./examples/word_count                      # built-in text: one mapper per split, 2 reducers
./examples/word_count mybook.txt 4 4       # your file; at most 4 mappers at a time; 4 reducers
./examples/inverted_index - 3 2 cat dog    # built-in documents, then search for "cat" and "dog"
./examples/matrix_multiply 30 20 25 4 4    # 30x20 times 20x25; at most 4 mappers at a time; 4 reducers
```

### Using the library in your own program

```c
#include "mapreduce.h"

static void my_map(const char *input, size_t len, mr_emitter_t *out, void *ctx) {
    /* look at the bytes of ONE split; call mr_emit(out, key, key_len, value, value_len) */
}
static void my_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                      size_t num_values, mr_emitter_t *out, void *ctx) {
    /* all values of ONE key; emit result pairs with mr_emit */
}

int main(void) {
    mr_job_t *job = mr_job_create();
    mr_job_set_map(job, my_map, NULL);
    mr_job_set_reduce(job, my_reduce, NULL);
    mr_job_set_reducers(job, 4);
    mr_job_set_input_file(job, "data.txt");          /* or mr_job_set_input(job, buf, len) */
    if (mr_job_run(job) != MR_OK) { /* handle the error */ }
    mr_job_write_output(job, "-");                   /* key<TAB>value lines on stdout */
    mr_job_destroy(job);
    return 0;
}
```

Compile it with `gcc -Iinclude myprog.c -L. -lmr -pthread -o myprog` (after `make libmr.a`).
Full function reference: `documentation/API.md`. A complete small program: `examples/word_count.c`.

### Memory and sanitizer checks

```
make asan        # everything rebuilt with AddressSanitizer + UBSan, run, cleaned up
make tsan        # ThreadSanitizer on the threaded tests and applications (race detector)
make valgrind    # leak check of the tests under Valgrind
make clean       # remove everything that was built (also inside simulation/)
```

### Worker analysis (how many mappers and reducers?)

```
make analysis                      # ./examples/benchmark  (2,000,000 words, 3 runs, ~2 minutes)
./examples/benchmark 2000000 5     # the settings used for results/benchmark_2M_words.txt
./examples/benchmark 500000 3      # smaller and faster
```

Method, tables and conclusions: `documentation/ANALYSIS.md`.

### The simulation

```
make sim            # builds simulation/ (its own library, demo and tests)
make sim-test       # runs the simulation's 6 test programs
./simulation/examples/midterm_demo [split_size] [mappers] [partitions]
```

See `simulation/README.md`. It shows splits, each mapper's work, the intermediate pairs and the
partitions, as printed tables. It is independent of the library.

## Tests

Nine test programs in `tests/` (details and what each one shows: `documentation/TESTING.md`):
normal, boundary, invalid, empty, large and failure cases, thread tests, memory tests, and a test
that makes `malloc` fail on purpose at every possible point. `make test` runs them all.

## Memory ownership (short version; full table in `documentation/DESIGN.md`)

| What | Who allocates | Who owns / frees |
|---|---|---|
| the job | `mr_job_create` | you, with `mr_job_destroy` (this also frees the output) |
| input given with `mr_job_set_input` | you | you; it is **not copied**, keep it alive until `mr_job_run` returns |
| input read with `mr_job_set_input_file` | the job | the job |
| pairs you `mr_emit` | the library **copies** key and value | the library; you keep your own buffers |
| `values` given to `reduce` | the library | the library; valid only during that call |
| `mr_job_output` | the job | the job; valid until the next run or destroy |

If `mr_job_run` fails, every intermediate buffer is freed and the output is empty.

## Error handling

Every function that can fail returns `mr_error_t` (`MR_OK`, `MR_ERR_NULL`, `MR_ERR_NO_MEMORY`,
`MR_ERR_INVALID`, `MR_ERR_IO`, `MR_ERR_CALLBACK`, `MR_ERR_STATE`, `MR_ERR_THREAD`);
`mr_error_string` gives text. A map/reduce function reports failure with `mr_emit_fail`.

## Limitations

- Threads in one process, **not** several computers; no network, no distributed file system.
- All data lives in memory (input, intermediate pairs and output must fit in RAM).
- The shuffle sorts the pairs; sorting is the slowest phase (see the analysis). No combiner yet.
- Threads are created per phase and joined (simple and safe; a persistent pool would save a little).
- Your `map`/`reduce` functions run in several threads at once and must not change shared data
  without their own locking.
- No retry: if one task fails, the job stops and returns the error.
- Values are plain bytes; applications write numbers as text and parse them in `reduce`.
- Examples read words as letters/digits in lower case and cut words longer than 127 characters.

## Division of work

_<fill in: who did which module, applications, tests, analysis and documentation>_

## Attribution

The hash is FNV-1a (a published algorithm). The design follows the MapReduce paper (Dean and
Ghemawat, 2004) and the thread-based style of Phoenix. This project was developed with AI
assistance (Claude); declare that as your course requires, and only describe the parts you have
read, run and can explain.
