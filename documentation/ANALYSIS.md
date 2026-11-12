# Worker analysis: how many mappers and reducers?

## Method
* Program: `examples/benchmark.c` (word count, built only from the public API). Run with
  `./examples/benchmark 2000000 5` (saved output: `results/benchmark_2M_words.txt`).
* **Meaning of "mappers" in the tables:** the library runs one mapper per split, so the mapper count is
  fixed by the split size (Table 3). The "mappers" column of Tables 1 and 2 is the limit of mapper threads
  running **at the same time** (`mr_job_set_mappers`), which is what varies the parallelism.
* Input: 2,000,000 generated words (16 MB), 5,000 different words with a skewed (Zipf-like)
  frequency, 10 words per line, always the same text (fixed seed).
* Every setting is run 5 times; the table shows mean ± standard deviation of the total time.
  **Every run is checked**: the counts must add up to 2,000,000, so a fast but wrong result is
  impossible.
* Time is `CLOCK_MONOTONIC` inside the library (`mr_job_stats`): split + map + shuffle + reduce.
  Reading the file and creating the text are not included.
* Machine: **2 virtual CPUs**, Intel Xeon 2.1 GHz, 8 GB RAM, Linux, gcc -O2. With 2 cores a
  speed-up above 2 would be impossible from parallelism alone. **Re-run `make analysis` on
  your own computer** and use your numbers; they will differ (more cores = more speed-up).

## Table 1: mappers = reducers = w (split size 256 KB)

| w | total ms (mean ± sd) | speed-up | efficiency | map ms | shuffle ms | reduce ms |
|---|---|---|---|---|---|---|
| 1 | 1385.9 ± 81.9 | 1.00 | 100% | 246.1 | 1076.1 | 63.6 |
| 2 | 742.2 ± 59.4 | 1.87 | 93% | 173.5 | 533.4 | 35.2 |
| 3 | 716.3 ± 86.2 | 1.93 | 64% | 194.8 | 478.6 | 42.9 |
| 4 | 749.7 ± 76.1 | 1.85 | 46% | 196.5 | 507.7 | 45.4 |
| 6 | 699.9 ± 68.7 | 1.98 | 33% | 213.3 | 444.9 | 41.7 |
| 8 | 691.2 ± 64.7 | 2.00 | 25% | 201.1 | 446.7 | 43.4 |

speed-up = time(1) / time(w); efficiency = speed-up / w.

## Table 2: total ms, mappers (rows) x reducers (columns)

| | red=1 | red=2 | red=4 | red=8 |
|---|---|---|---|---|
| map=1 | 1432.4 | 824.4 | 731.1 | 690.5 |
| map=2 | 1453.3 | 747.1 | 768.9 | 811.8 |
| map=4 | 1478.9 | 831.3 | 764.6 | 800.2 |
| map=8 | 1485.2 | 855.6 | 804.6 | 691.5 |

## Table 3: split size (2 mappers, 2 reducers)

| split size | splits | total ms (mean ± sd) |
|---|---|---|
| 4 KB | 4086 | 784.7 ± 82.3 |
| 16 KB | 1022 | 738.3 ± 93.5 |
| 64 KB | 256 | 762.9 ± 53.8 |
| 256 KB | 64 | 760.7 ± 59.9 |
| 1 MB | 16 | 730.4 ± 59.7 |
| 4 MB | 4 | 796.8 ± 103.7 |
| 16 MB | 1 | 789.4 ± 87.1 |

## What the numbers say

1. **Parallelism helps, up to the number of cores.** Going from 1 to 2 workers nearly halves the
   time (1.87x, 93% efficient). More than 2 workers gains almost nothing (1.85x-2.00x), because
   this machine has 2 cores. Beyond the core count, extra threads only add scheduling work.
   The differences between w = 2, 3, 4, 6, 8 (691-750 ms) are about the size of one standard
   deviation (60-86 ms), so they are **not** meaningful differences.
2. **The reducers (shuffle) matter much more than the mappers.** In the 1-worker run the shuffle
   (gather + sort) takes 78% of the time, the map phase 18%, the reduce phase 5%. Table 2 shows
   it: with 1 reducer the time is about 1450 ms whatever the number of mappers; with 2 or more
   reducers it drops to 690-860 ms. More mappers cannot help because the map phase is only a small
   part of the job.
3. **Why the shuffle scales:** every reducer sorts only its own partition, and partitions are
   independent, so two partitions are sorted at the same time. (Sorting n/2 items also needs a
   little less work than n items.)
4. **Where the optimum is.** On this 2-core machine: **mappers and reducers each about equal to
   the number of cores (2)**, with reducers >= cores so every core has a partition to sort. Using
   more brings no measurable gain and costs memory (more lists); using fewer wastes cores. A
   rule of thumb for the library: `mappers = reducers = number of CPU cores`; use more reducers
   only when keys are unevenly distributed (one popular key makes one partition much larger).
5. **Split size hardly matters** between 16 KB and 4 MB (all within one standard deviation).
   Very small splits (4 KB, 4086 tasks) cost a little more task handling; a single huge split
   (16 MB) removes all mapper parallelism and is slowest. Practical choice: enough splits so that
   every mapper has several (here 64 KB - 1 MB).
6. **Scalability.** Efficiency falls as w grows (93% -> 25%) because the cores are used up and
   because some work stays sequential: splitting, the final gather and sort of the output, and
   the hand-over between phases (Amdahl's law). Skewed word frequencies can also make one
   partition larger than the others (not measured separately here), which limits how well
   reducers can be balanced.

## A bug that the analysis found (and that was fixed)

The first benchmark showed that **more mappers did not make the map phase faster** (about 230 ms
with 1 and with 2 mappers), although a test with only tokenizing (no `mr_emit`) did scale.
The cause was *false sharing*: the table of lists had the list headers of neighbouring splits next
to each other, in the same CPU cache line, and every `mr_emit` writes to its list header. Two
threads on neighbouring splits kept taking that cache line from each other. Fix: each map task now
fills a local array of lists and copies it into the table once at the end (`src/mr_map.c`). The map
phase then became measurably faster with 2 mappers (about 158 ms -> 116 ms in a map-only test).
Sorting, not mapping, is still the bottleneck.

## Honest limitations of this analysis

* Only 2 CPU cores were available, so scalability beyond 2 cores could not be measured. The
  trend up to 2 workers is real; the flat part after it is the core limit, not a library limit.
* One workload (word count, skewed words) and one input size. Jobs with heavy `map` or `reduce`
  computation (for example matrix multiplication) would benefit more from extra mappers.
* The numbers come from a shared virtual machine; timings vary from run to run (see the standard
  deviations). Repeat on your computer and report your own table and machine.

## How to reproduce and extend

```
make analysis                         # default: 2,000,000 words, 3 runs
./examples/benchmark 4000000 5        # bigger input
nproc                                 # how many cores you have
```
Compare your result with these tables; if you have 4 or 8 cores, you should see the speed-up
continue up to that number of workers. Add your table (with `nproc` and the CPU model) to the report.
