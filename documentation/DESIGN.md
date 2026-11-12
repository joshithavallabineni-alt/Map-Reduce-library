# Design

## 1. The big picture

You give the library a `map` function and a `reduce` function. The library runs the job in four
phases. Three of them run on several threads.

```
 input (memory or file)
   |
   |  1. SPLIT      (one thread)        mr_split_input
   v
 splits  S0 S1 S2 S3 ...               a split = pointer + length into the input (no copy)
   |
   |  2. MAP        (mapper threads)    mr_map_phase
   |     each split -> your map() -> mr_emit(key,value)
   |     every emitted pair is put straight into the list of its partition
   |     partition = FNV-1a(key) mod number_of_reducers
   v
 map table: one list per (split, partition)
   |
   |  3. SHUFFLE    (one task per partition, parallel)   mr_shuffle_phase
   |     partition r gathers its list from EVERY split, then sorts by key
   v
 partitions P0 P1 ... (each sorted, equal keys next to each other)
   |
   |  4. REDUCE     (one task per partition, parallel)   mr_reduce_phase
   |     walk along the sorted array; every run of equal keys = one group
   |     your reduce(key, all values of that key) -> mr_emit(result)
   v
 final output: all results, sorted by key (then value)
```

Every phase ends with all its threads joined. A phase starts only when the one before is
completely finished, so no thread ever reads data that another thread is still writing.

**Data flow of one word.** In word count the word `the` appears in splits 0, 3 and 5. Three
different mapper threads emit `("the","1")`. All three pairs hash to the same partition, say 2.
During the shuffle, task 2 collects them (from splits 0, 3, 5) and sorts partition 2, so the three
`the` pairs are neighbours. Reducer task 2 sees the run `the,1  the,1  the,1`, calls
`reduce("the", [1,1,1])`, and the user function emits `("the","3")`.

## 2. Thread management (`src/mr_pool.c`)

There is one helper, `mr_run_parallel(num_tasks, num_workers, fn, arg, &used)`, used by all three
parallel phases.

* A **task** is a number `0 .. num_tasks-1`. In the map phase a task is a split; in the shuffle and
  reduce phases a task is a partition.
* The helper creates `min(num_workers, num_tasks)` POSIX threads. Each thread loops:
  *lock the mutex, take the next task number from a shared counter, add 1, unlock, run the task.*
  When no number is left the thread ends. So a thread that finishes early takes more work:
  this is **dynamic load balancing**, and uneven splits do not leave threads idle.
* The mutex is held only for the moment of taking a number. The tasks themselves need **no locks**:
  each task writes only into its own slot (its own list or its own output list). That is the central
  thread-safety idea: *share nothing while working, combine after the join.*
* `pthread_join` is the synchronisation point. It guarantees that everything a worker wrote is
  visible to the main thread afterwards.
* The first error is stored under the mutex. After an error no new task is started; running tasks
  finish, all threads are joined, and the error is returned.
* One worker asked for, or `pthread_create` fails: the same loop runs in the calling thread, so
  the job still works (slower) instead of failing.
* **One mapper per split (Hadoop style):** the number of mappers is the number of splits (input size / split size, rounded up); `mr_job_stats()->mappers_used` equals `num_splits`. At most `MR_MAX_WORKERS` (64) mapper threads run at the same time (a thread per split for thousands of splits would exhaust memory and thread limits); a thread that finishes takes the next split. `mr_job_set_mappers(n)` is optional and only lowers that limit (0 = no limit).
* The number of reducers is chosen by the user and is limited to `MR_MAX_WORKERS` (64).

**Why not locks around shared lists?** It would make every `mr_emit` wait for the others. Giving every
(split, partition) its own list removes the need. Because neighbouring list headers share a CPU cache
line, each task fills a *local* array of lists and copies it into the table once at the end;
without this, threads on neighbouring splits slowed each other down on every emit ("false sharing",
found with the benchmark, see `ANALYSIS.md`).

**Determinism.** Threads finish in a different order every run, yet the output is always the same:
the values of a key are sorted before `reduce`, and the final output is sorted. Tests check that
1, 2, 4, 8 simultaneous mappers and reducers give identical output.

## 3. Data structures (`include/mapreduce.h`)

| Structure | Meaning |
|---|---|
| `mr_kv_t` | one pair: `key`, `key_len`, `value`, `value_len`. Byte buffers with explicit lengths (a key may contain `\0`); the library adds one hidden `\0` so `%s` works. |
| `mr_kv_list_t` | growing array of pairs (`items`, `count`, `capacity`, doubles when full). Owns every buffer in it. |
| `mr_split_t`, `mr_split_list_t` | a split = pointer + length into the input; a list of splits. |
| `mr_emitter_t` | handed to map and reduce functions: the lists to write into and the first error. |
| `mr_job_t` (opaque) | settings (functions, ctx, worker counts, split size and mode), input, output, statistics. |
| `mr_job_stats_t` | counts and times of the last run (split/map/shuffle/reduce/total ms). |

## 4. Algorithms

| Step | Algorithm | Cost |
|---|---|---|
| Split | cut every `split_size` bytes; in word/line mode move the cut forward to just after white space / `\n` so nothing is cut | O(number of splits + cut distance) |
| Partition | FNV-1a 64-bit hash of the key bytes, mod number of partitions | O(key length) |
| Map | your function; `mr_emit` hashes the key and appends a copy to the right list | O(input) |
| Shuffle | per partition: move the pairs of all splits into one array (only the small records are copied; key/value buffers stay where they are), `qsort` by (key, value) | O(n log n) per partition |
| Group | one pass over the sorted array; a run of equal keys is a group, passed to `reduce` as a pointer into the array (no copy) | O(n) |
| Output | move every reducer's results into one array and sort it | O(m log m) |

Sorting-based grouping was chosen over a hash table because it is simple, makes the value order
deterministic, and the same sorted array gives sorted output for free. Its price is that the shuffle
is the slowest phase (see the analysis); a hash-based grouping is a possible improvement.

## 5. Memory ownership (who allocates, who owns, who frees, what if it fails)

| Object | Allocated by | Owned by | Freed by | If something fails |
|---|---|---|---|---|
| `mr_job_t` | `mr_job_create` | caller | `mr_job_destroy` | create returns NULL |
| input from `mr_job_set_input` | caller | caller | caller (after `mr_job_run` returns) | not copied, so nothing to free in the library |
| input from `mr_job_set_input_file` | job | job | job (next input / destroy) | MR_ERR_IO / MR_ERR_NO_MEMORY, nothing kept |
| split list | `mr_job_run` | `mr_job_run` | `mr_job_run` | freed on every path |
| pair emitted by `mr_emit` | library (copies key, value) | the list it is in | the list owner | emit stores the error, job returns it |
| map table (split x partition lists) | map phase | `mr_job_run` | `mr_job_run` after the shuffle | freed on error |
| partitions (sorted arrays) | shuffle phase (pairs *moved*, not copied) | `mr_job_run` | `mr_job_run` after the reduce | freed on error |
| `values` passed to `reduce` | library | library | library | valid only during the call; copy what you need |
| final output | reduce phase | the job | next `mr_job_run` or `mr_job_destroy` | empty after a failed run |

Moving instead of copying: the shuffle and the final output move the pair records between arrays;
the key and value buffers are allocated once (in `mr_emit`) and freed once (with the output).
`test_alloc_fail` makes the n-th allocation fail for n = 1, 2, 3, ... and checks that the job always
returns `MR_ERR_NO_MEMORY` cleanly; Valgrind/ASan show no leak on any of those paths.

## 6. Error handling

Every public function returns `mr_error_t` (or a pointer that may be NULL, documented in the
header). Bad arguments, wrong call order (`MR_ERR_STATE`), file problems (`MR_ERR_IO`), allocation
failure, thread problems, and failures reported by user functions (`mr_emit_fail`, a NULL key)
all give an error code and never a crash. A failed job leaves the output empty.

## 7. Design decisions

* **Library first.** `include/mapreduce.h` is the only header users need; internals are in
  `src/mr_internal.h`.
* **Hadoop-like API.** A job object with setters (`mr_job_set_map`, `..._reducers`, ...) and `run`.
  `ctx` pointers give map/reduce functions parameters without global variables (needed by
  matrix multiplication).
* **Threads, not processes.** Shared memory makes moving data between phases cheap; the price is that
  user functions must be thread-safe.
* **Map-side partitioning.** Hashing at emit time means the shuffle needs no scan of foreign data.
* **Pairs are copied once** (at `mr_emit`) and moved afterwards.
* **Split modes.** `MR_SPLIT_LINES` (default) keeps records whole; `MR_SPLIT_WORDS` and
  `MR_SPLIT_BYTES` exist and are tested. This fixes the midterm's "a split can cut a word" limitation.
* **Reuse of the midterm work:** the key-value list, the splitter, the FNV-1a partitioning, the
  error codes and the test helpers come from the midterm code. Added: `ctx`, split modes, sorting,
  the job object, threads, shuffle, reduce, file input/output, statistics.
