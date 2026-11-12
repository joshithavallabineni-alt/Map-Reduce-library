/* mr_shuffle.c - the shuffle phase.
 *
 * Reducer r must see EVERY pair that belongs to partition r, from every
 * split. Task r therefore
 *   1. gathers list [s][r] of every split s into one array (the pairs are
 *      MOVED: only the small mr_kv_t records are copied, the key and value
 *      buffers keep their address, so no malloc and no byte copy happens),
 *   2. sorts the array by (key, value), so equal keys become neighbours.
 * Grouping then is just "walk along the array" (see mr_reduce.c).
 * The tasks of different partitions are independent, so they run in parallel. */
#include <stdlib.h>
#include <string.h>

#include "mr_internal.h"

typedef struct {
    size_t         num_splits;
    size_t         num_reducers;
    mr_kv_list_t  *map_table;
    mr_kv_list_t  *partitions;
} shuffle_args_t;

static mr_error_t shuffle_task(size_t r, size_t worker_index, void *arg)
{
    (void)worker_index;
    shuffle_args_t *a = arg;
    size_t num_r = a->num_reducers;

    size_t total = 0;
    for (size_t s = 0; s < a->num_splits; s++) total += a->map_table[s * num_r + r].count;
    if (total == 0) return MR_OK;

    mr_kv_t *items = malloc(total * sizeof(mr_kv_t));
    if (items == NULL) return MR_ERR_NO_MEMORY;

    size_t n = 0;
    for (size_t s = 0; s < a->num_splits; s++) {
        mr_kv_list_t *src = &a->map_table[s * num_r + r];
        if (src->count > 0) memcpy(items + n, src->items, src->count * sizeof(mr_kv_t));
        n += src->count;
        free(src->items);              /* the pairs now belong to "items" */
        mr_kv_list_init(src);
    }

    mr_kv_list_t *part = &a->partitions[r];
    part->items = items;
    part->count = total;
    part->capacity = total;
    mr_kv_list_sort(part);
    return MR_OK;
}

mr_error_t mr_shuffle_phase(const struct mr_job *job, mr_kv_list_t *map_table,
                            size_t num_splits, mr_kv_list_t **out_partitions)
{
    *out_partitions = NULL;
    size_t r = job->num_reducers;
    mr_kv_list_t *partitions = calloc(r, sizeof(mr_kv_list_t));
    if (partitions == NULL) return MR_ERR_NO_MEMORY;

    if (num_splits > 0) {
        shuffle_args_t args = { num_splits, r, map_table, partitions };
        mr_error_t err = mr_run_parallel(r, r, shuffle_task, &args, NULL);
        if (err != MR_OK) {
            mr_free_map_table(partitions, r);
            return err;
        }
    }
    *out_partitions = partitions;
    return MR_OK;
}
