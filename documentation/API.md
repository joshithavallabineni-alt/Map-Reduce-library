# API reference (`include/mapreduce.h`)

All functions returning `mr_error_t` return `MR_OK` (0) on success.

## Errors
`MR_OK`, `MR_ERR_NULL` (a required pointer is NULL), `MR_ERR_NO_MEMORY`, `MR_ERR_INVALID` (value not
allowed), `MR_ERR_IO` (file problem), `MR_ERR_CALLBACK` (your function called `mr_emit_fail`),
`MR_ERR_STATE` (call made in the wrong state, e.g. run without a reduce function),
`MR_ERR_THREAD`. `const char *mr_error_string(mr_error_t)` returns a text (never NULL).

## Your two functions
```c
typedef void (*mr_map_fn)(const char *input, size_t input_len, mr_emitter_t *out, void *ctx);
typedef void (*mr_reduce_fn)(const char *key, size_t key_len, const mr_kv_t *values,
                             size_t num_values, mr_emitter_t *out, void *ctx);
```
* `map` is called once per split (**one mapper per split**), from a mapper thread. `input` is the split, not the whole input.
* `reduce` is called once per distinct key, from a reducer thread, with all its values
  (`values[i].value`, `values[i].value_len`; sorted by value bytes).
* Both emit with `mr_emit`. They run in several threads at the same time: do not change shared data
  without your own locking. Reading `ctx` is fine.

```c
void mr_emit(mr_emitter_t *out, const char *key, size_t key_len,
             const char *value, size_t value_len);   /* library copies key and value */
void mr_emit_fail(mr_emitter_t *out);                /* job returns MR_ERR_CALLBACK */
```
A NULL key is an error (`MR_ERR_NULL`); the first error is remembered and later emits do nothing.

## Job
```c
mr_job_t *mr_job_create(void);              /* defaults: 1 reducer, 64 KB splits, line mode; mappers = one per split */
void      mr_job_destroy(mr_job_t *job);    /* frees job, output, owned input; NULL is fine */

mr_error_t mr_job_set_map(job, fn, ctx);
mr_error_t mr_job_set_reduce(job, fn, ctx);
mr_error_t mr_job_set_mappers(job, n);      /* OPTIONAL: max mapper threads at the same time,
                                               0..64; 0 (default) = no limit. The number of
                                               mappers is always one per split. */
mr_error_t mr_job_set_reducers(job, n);     /* 1..64; also the number of partitions */
mr_error_t mr_job_set_split_size(job, bytes);          /* > 0 */
mr_error_t mr_job_set_split_mode(job, MR_SPLIT_BYTES | MR_SPLIT_WORDS | MR_SPLIT_LINES);
mr_error_t mr_job_set_input(job, data, len);           /* not copied: keep alive until run returns */
mr_error_t mr_job_set_input_file(job, path);           /* job reads and owns a copy */

mr_error_t mr_job_run(job);                 /* runs split, map, shuffle, reduce */
const mr_kv_list_t   *mr_job_output(job);   /* sorted by key; valid until next run/destroy */
const mr_job_stats_t *mr_job_stats(job);    /* counts and times of the last run */
mr_error_t mr_job_write_output(job, path);  /* "key<TAB>value" lines; "-" = stdout */
```
`mr_job_run` errors: `MR_ERR_STATE` (no map or reduce function, or no input), anything your
functions emit, `MR_ERR_NO_MEMORY`, `MR_ERR_THREAD`. On error the output is empty and all
intermediate memory is freed. A job can be run again.

## Building blocks (also usable on their own)
```c
void mr_kv_list_init(mr_kv_list_t *);   mr_error_t mr_kv_list_add(list, key, klen, value, vlen);
void mr_kv_list_free(mr_kv_list_t *);   void mr_kv_list_sort(list);   int mr_kv_compare(a, b);
mr_error_t mr_split_input(input, len, split_size, mode, &out);   void mr_split_list_free(&list);
size_t mr_partition_of(key, key_len, num_partitions);            /* FNV-1a mod n */
```
