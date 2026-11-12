/* mr_map.c - emitting pairs, and the map phase.
 *
 * Every split is one task. The thread that runs the task calls the user's map
 * function; each mr_emit call copies the pair into the list of the partition
 * its key belongs to ("map-side partitioning"). The table has one list per
 * (split, partition), so threads never write to the same list and no lock is
 * needed while mapping. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mr_internal.h"

void mr_emit(mr_emitter_t *out, const char *key, size_t key_len,
             const char *value, size_t value_len)
{
    if (out == NULL || out->error != MR_OK) return;   /* first error sticks */
    if (out->buckets == NULL || out->num_buckets == 0) {
        out->error = MR_ERR_STATE;
        return;
    }
    if (key == NULL) {
        out->error = MR_ERR_NULL;
        return;
    }
    size_t bucket = 0;
    if (out->num_buckets > 1) bucket = mr_partition_of(key, key_len, out->num_buckets);
    mr_error_t err = mr_kv_list_add(&out->buckets[bucket], key, key_len, value, value_len);
    if (err != MR_OK) out->error = err;
}

void mr_emit_fail(mr_emitter_t *out)
{
    if (out == NULL || out->error != MR_OK) return;
    out->error = MR_ERR_CALLBACK;
}

void mr_free_map_table(mr_kv_list_t *table, size_t num_lists)
{
    if (table == NULL) return;
    for (size_t i = 0; i < num_lists; i++) mr_kv_list_free(&table[i]);
    free(table);
}

typedef struct {
    const struct mr_job   *job;
    const mr_split_list_t *splits;
    mr_kv_list_t          *table;
} map_args_t;

static mr_error_t map_task(size_t split_index, size_t worker_index, void *arg)
{
    (void)worker_index;
    map_args_t *a = arg;
    size_t r = a->job->num_reducers;
    const mr_split_t *split = &a->splits->splits[split_index];

    /* The lists are filled in a LOCAL array and copied into the shared table
     * afterwards. Why: the list headers of neighbouring splits sit next to each
     * other in the table (same cache line), and every mr_emit changes a header.
     * Two threads working on neighbouring splits would then fight over that cache
     * line on every single emit ("false sharing"), and more mappers would not be
     * faster. Measured: this change made the map phase scale with the cores. */
    mr_kv_list_t local[MR_MAX_WORKERS];
    for (size_t i = 0; i < r; i++) mr_kv_list_init(&local[i]);

    mr_emitter_t em;
    em.buckets = local;
    em.num_buckets = r;
    em.error = MR_OK;

    a->job->map_fn(split->data, split->length, &em, a->job->map_ctx);

    /* Hand the lists over (also after an error, so they are freed with the table). */
    memcpy(&a->table[split_index * r], local, r * sizeof(mr_kv_list_t));
    return em.error;
}

mr_error_t mr_map_phase(const struct mr_job *job, const mr_split_list_t *splits,
                        mr_kv_list_t **out_table, size_t *pairs_emitted,
                        size_t *mappers_used)
{
    *out_table = NULL;
    *pairs_emitted = 0;
    *mappers_used = 0;
    if (splits->count == 0) return MR_OK;

    size_t r = job->num_reducers;
    if (splits->count > SIZE_MAX / r / sizeof(mr_kv_list_t)) return MR_ERR_NO_MEMORY;
    size_t lists = splits->count * r;
    mr_kv_list_t *table = calloc(lists, sizeof(mr_kv_list_t));  /* all-zero = empty lists */
    if (table == NULL) return MR_ERR_NO_MEMORY;

    map_args_t args = { job, splits, table };

    /* Hadoop style: ONE MAPPER PER SPLIT. Every split is its own map task, so the
     * number of mappers is the number of splits (input size / split size).
     * Threads: one per mapper, but never more than MR_MAX_WORKERS (64) at the same
     * time; when there are more splits than that, a thread that finishes takes the
     * next split. mr_job_set_mappers(job, n) can lower the limit (0 = no limit). */
    size_t threads = splits->count;
    if (job->num_mappers > 0 && job->num_mappers < threads) threads = job->num_mappers;
    if (threads > MR_MAX_WORKERS) threads = MR_MAX_WORKERS;

    mr_error_t err = mr_run_parallel(splits->count, threads, map_task, &args, NULL);
    if (err != MR_OK) {
        mr_free_map_table(table, lists);
        return err;
    }
    *mappers_used = splits->count;          /* number of mappers = number of splits */

    for (size_t i = 0; i < lists; i++) *pairs_emitted += table[i].count;
    *out_table = table;
    return MR_OK;
}
