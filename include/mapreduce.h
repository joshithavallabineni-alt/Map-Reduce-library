/*
 * mapreduce.h - public API of the MapReduce library (multithreaded version)
 *
 * A user writes two functions:
 *     map    : reads one piece of the input and emits (key, value) pairs
 *     reduce : receives one key with ALL its values and emits result pairs
 * The library does everything in between:
 *
 *   input --split--> splits --map (threads)--> partitioned pairs
 *         --shuffle: gather, sort, group (threads)--> groups
 *         --reduce (threads)--> final output
 *
 * Typical use:
 *     mr_job_t *job = mr_job_create();
 *     mr_job_set_map(job, my_map, NULL);
 *     mr_job_set_reduce(job, my_reduce, NULL);
 *     mr_job_set_reducers(job, 4);          // mappers: one per split, automatically
 *     mr_job_set_input(job, text, text_len);
 *     if (mr_job_run(job) == MR_OK) { ...read mr_job_output(job)... }
 *     mr_job_destroy(job);
 *
 * THREAD SAFETY: the map and reduce functions are called from several
 * threads at the same time. They must not change shared data (for example
 * the ctx they are given) without their own locking. Reading ctx is fine.
 */
#ifndef MAPREDUCE_H
#define MAPREDUCE_H

#include <stddef.h>

/* Largest number of mappers (and of reducers) a job accepts. */
#define MR_MAX_WORKERS 64

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    MR_OK = 0,          /* success */
    MR_ERR_NULL,        /* a required pointer argument was NULL */
    MR_ERR_NO_MEMORY,   /* malloc/realloc failed */
    MR_ERR_INVALID,     /* an argument has a value that is not allowed */
    MR_ERR_IO,          /* a file could not be read or written */
    MR_ERR_CALLBACK,    /* the user's map/reduce function reported failure */
    MR_ERR_STATE,       /* the job is not ready for this call */
    MR_ERR_THREAD       /* a thread could not be used */
} mr_error_t;

/* Returns a short description of an error code (never NULL). */
const char *mr_error_string(mr_error_t err);

/* ------------------------------------------------------------------ */
/* Key-value pairs                                                     */
/* ------------------------------------------------------------------ */

/* One pair. key and value are byte buffers with explicit lengths, so a key
 * may contain '\0' bytes. The library keeps its own copy of the data and adds
 * one extra '\0' after it (not counted in the length) so the buffers can be
 * printed with %s. */
typedef struct {
    char  *key;
    size_t key_len;
    char  *value;
    size_t value_len;
} mr_kv_t;

/* A growing array of pairs. Owns every key and value buffer in it. */
typedef struct {
    mr_kv_t *items;
    size_t   count;
    size_t   capacity;
} mr_kv_list_t;

/* Makes an empty list. Call this before using a list. */
void mr_kv_list_init(mr_kv_list_t *list);

/* Adds a COPY of key and value to the end of the list.
 * Errors: MR_ERR_NULL (list or key NULL, or value NULL while value_len > 0),
 *         MR_ERR_NO_MEMORY. On error the list is unchanged. */
mr_error_t mr_kv_list_add(mr_kv_list_t *list,
                          const char *key, size_t key_len,
                          const char *value, size_t value_len);

/* Frees every key, every value and the array; leaves an empty list.
 * Safe on NULL and on a list that was already freed. */
void mr_kv_list_free(mr_kv_list_t *list);

/* Orders two pairs: by key bytes (a shorter key is smaller when it is a
 * prefix of the other), then by value bytes. Returns <0, 0 or >0. */
int mr_kv_compare(const mr_kv_t *a, const mr_kv_t *b);

/* Sorts a list with mr_kv_compare. */
void mr_kv_list_sort(mr_kv_list_t *list);

/* ------------------------------------------------------------------ */
/* Input splitting                                                     */
/* ------------------------------------------------------------------ */

/* Where a split may end:
 *   MR_SPLIT_BYTES : exactly split_size bytes (may cut a word or a line)
 *   MR_SPLIT_WORDS : the cut is moved forward to just after white space,
 *                    so no word is ever cut
 *   MR_SPLIT_LINES : the cut is moved forward to just after a '\n',
 *                    so every split holds whole lines */
typedef enum {
    MR_SPLIT_BYTES = 0,
    MR_SPLIT_WORDS,
    MR_SPLIT_LINES
} mr_split_mode_t;

/* A split is a view into the input buffer: nothing is copied. */
typedef struct {
    const char *data;
    size_t      length;
} mr_split_t;

typedef struct {
    mr_split_t *splits;
    size_t      count;
} mr_split_list_t;

/* Cuts the input into pieces of about split_size bytes. The splits cover the
 * input exactly once, in order. Empty input gives 0 splits.
 * Errors: MR_ERR_NULL, MR_ERR_INVALID (split_size 0 or unknown mode),
 *         MR_ERR_NO_MEMORY. *out is always left valid (empty on error). */
mr_error_t mr_split_input(const char *input, size_t input_len,
                          size_t split_size, mr_split_mode_t mode,
                          mr_split_list_t *out);

/* Frees the splits array (not the input). Safe on NULL / twice. */
void mr_split_list_free(mr_split_list_t *list);

/* ------------------------------------------------------------------ */
/* Partitioning                                                        */
/* ------------------------------------------------------------------ */

/* The partition (= reducer number) a key belongs to: FNV-1a hash of the key
 * bytes, mod num_partitions. Same key, same partition, every time.
 * Returns 0 when num_partitions is 0. */
size_t mr_partition_of(const char *key, size_t key_len, size_t num_partitions);

/* ------------------------------------------------------------------ */
/* Emitting pairs (used inside map and reduce functions)               */
/* ------------------------------------------------------------------ */

/* Handed to map and reduce functions. The fields are private. */
typedef struct mr_emitter {
    mr_kv_list_t *buckets;      /* one list per partition (map) or one (reduce) */
    size_t        num_buckets;
    mr_error_t    error;        /* first error; later emits do nothing */
} mr_emitter_t;

/* Emits one pair; the library copies key and value. A NULL key is an error
 * (MR_ERR_NULL); a NULL value needs value_len 0. The first error is
 * remembered and makes the job fail with that error. */
void mr_emit(mr_emitter_t *out, const char *key, size_t key_len,
             const char *value, size_t value_len);

/* Lets a map/reduce function report its own failure (the job returns
 * MR_ERR_CALLBACK). */
void mr_emit_fail(mr_emitter_t *out);

/* ------------------------------------------------------------------ */
/* User functions                                                      */
/* ------------------------------------------------------------------ */

/* Called once per split, from a mapper thread. input/input_len is the split;
 * call mr_emit for every pair to produce. ctx is what was given to
 * mr_job_set_map. */
typedef void (*mr_map_fn)(const char *input, size_t input_len,
                          mr_emitter_t *out, void *ctx);

/* Called once per distinct key, from a reducer thread. values[0..num_values-1]
 * are all pairs with this key (use .value and .value_len; they are sorted by
 * value bytes, so the order never depends on thread timing). Emit result
 * pairs with mr_emit. The pairs belong to the library: do not free them. */
typedef void (*mr_reduce_fn)(const char *key, size_t key_len,
                             const mr_kv_t *values, size_t num_values,
                             mr_emitter_t *out, void *ctx);

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

typedef struct mr_job mr_job_t;

/* Timing and counts of the last successful run (milliseconds). */
typedef struct {
    size_t num_splits;          /* splits made                               */
    size_t mappers_used;        /* mappers run = one per split = num_splits  */
    size_t reducers_used;       /* reducer threads actually used             */
    size_t intermediate_pairs;  /* pairs emitted by all map calls            */
    size_t groups;              /* distinct keys seen by reduce              */
    size_t output_pairs;        /* pairs in the final output                 */
    double split_ms;
    double map_ms;
    double shuffle_ms;          /* gather + sort + group per partition       */
    double reduce_ms;
    double total_ms;
} mr_job_stats_t;

/* Creates a job with defaults: 1 mapper, 1 reducer, split size 65536,
 * MR_SPLIT_LINES. Returns NULL if memory is short. Free with mr_job_destroy. */
mr_job_t *mr_job_create(void);

/* Frees the job, its output and any input it owns. Safe on NULL. */
void mr_job_destroy(mr_job_t *job);

/* Set the user functions. ctx is passed to every call (may be NULL). */
mr_error_t mr_job_set_map(mr_job_t *job, mr_map_fn fn, void *ctx);
mr_error_t mr_job_set_reduce(mr_job_t *job, mr_reduce_fn fn, void *ctx);

/* Number of reducers (1 .. MR_MAX_WORKERS). This is also the number of
 * partitions.
 *
 * MAPPERS (Hadoop style): there is ONE MAPPER PER SPLIT, so the number of
 * mappers is input size / split size (rounded up) and is not set by the user.
 * Mapper threads run at the same time, at most MR_MAX_WORKERS (64) of them; a
 * thread that finishes takes the next split. mr_job_set_mappers is OPTIONAL: it
 * lowers that limit to n threads at a time (it does not change the number of
 * mappers). n = 0 (the default) means "no limit except MR_MAX_WORKERS". */
mr_error_t mr_job_set_mappers(mr_job_t *job, size_t n);
mr_error_t mr_job_set_reducers(mr_job_t *job, size_t n);

/* Split size in bytes (> 0) and split mode. */
mr_error_t mr_job_set_split_size(mr_job_t *job, size_t bytes);
mr_error_t mr_job_set_split_mode(mr_job_t *job, mr_split_mode_t mode);

/* Input from memory. The bytes are NOT copied: the caller keeps the buffer
 * alive and unchanged until mr_job_run has returned. (NULL, 0) is an empty
 * input. */
mr_error_t mr_job_set_input(mr_job_t *job, const char *data, size_t len);

/* Input from a file. The job reads the whole file and owns the copy.
 * Errors: MR_ERR_IO (cannot open/read), MR_ERR_NO_MEMORY. */
mr_error_t mr_job_set_input_file(mr_job_t *job, const char *path);

/* Runs the whole job. May be called again (the old output is discarded).
 * Errors: MR_ERR_STATE (no map/reduce function or no input), anything the
 * user functions emit (MR_ERR_NULL, MR_ERR_CALLBACK), MR_ERR_NO_MEMORY.
 * On error the output is empty and everything is freed. */
mr_error_t mr_job_run(mr_job_t *job);

/* The final output: sorted by key, then by value, so it does not depend on
 * the number of mappers or reducers. Owned by the job: valid until the next
 * mr_job_run or mr_job_destroy. Empty before the first successful run. */
const mr_kv_list_t *mr_job_output(const mr_job_t *job);

/* Statistics of the last successful run (all zero before). */
const mr_job_stats_t *mr_job_stats(const mr_job_t *job);

/* Writes the output as lines "key<TAB>value". path "-" means stdout.
 * Errors: MR_ERR_STATE (job has not run), MR_ERR_IO. */
mr_error_t mr_job_write_output(const mr_job_t *job, const char *path);

#endif /* MAPREDUCE_H */
