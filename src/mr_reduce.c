/* mr_reduce.c - grouping and the reduce phase.
 *
 * A partition is sorted, so all pairs of one key are next to each other:
 *     the,1  the,1  the,1  cat,1  ...
 * Grouping = find each run of equal keys; the run is the "list of values" given
 * to the user's reduce function (no copy, the function gets a pointer into the
 * partition). One partition is one task; results go to the task's own output
 * list, so reducer threads never share a list. */
#include <stdlib.h>
#include <string.h>

#include "mr_internal.h"

typedef struct {
    const struct mr_job *job;
    mr_kv_list_t        *partitions;
    mr_kv_list_t        *results;    /* one output list per reducer */
    size_t              *groups;     /* groups seen per reducer     */
} reduce_args_t;

static int same_key(const mr_kv_t *a, const mr_kv_t *b)
{
    return a->key_len == b->key_len &&
           (a->key_len == 0 || memcmp(a->key, b->key, a->key_len) == 0);
}

static mr_error_t reduce_task(size_t r, size_t worker_index, void *arg)
{
    (void)worker_index;
    reduce_args_t *a = arg;
    const mr_kv_list_t *part = &a->partitions[r];

    mr_emitter_t em;
    em.buckets = &a->results[r];
    em.num_buckets = 1;
    em.error = MR_OK;

    size_t i = 0;
    while (i < part->count) {
        size_t j = i + 1;
        while (j < part->count && same_key(&part->items[i], &part->items[j])) j++;

        a->job->reduce_fn(part->items[i].key, part->items[i].key_len,
                          &part->items[i], j - i, &em, a->job->reduce_ctx);
        a->groups[r]++;
        if (em.error != MR_OK) return em.error;
        i = j;
    }
    return MR_OK;
}

mr_error_t mr_reduce_phase(struct mr_job *job, mr_kv_list_t *partitions,
                           size_t *groups, size_t *reducers_used)
{
    size_t r = job->num_reducers;
    *groups = 0;
    *reducers_used = 0;

    mr_kv_list_t *results = calloc(r, sizeof(mr_kv_list_t));
    size_t *group_counts = calloc(r, sizeof(size_t));
    if (results == NULL || group_counts == NULL) {
        free(results);
        free(group_counts);
        return MR_ERR_NO_MEMORY;
    }

    reduce_args_t args = { job, partitions, results, group_counts };
    mr_error_t err = mr_run_parallel(r, r, reduce_task, &args, reducers_used);

    size_t total = 0;
    if (err == MR_OK) {
        for (size_t i = 0; i < r; i++) {
            total += results[i].count;
            *groups += group_counts[i];
        }
        if (total > 0) {
            mr_kv_t *items = malloc(total * sizeof(mr_kv_t));
            if (items == NULL) {
                err = MR_ERR_NO_MEMORY;
            } else {
                /* Move every reducer's output into the job's output list. */
                size_t n = 0;
                for (size_t i = 0; i < r; i++) {
                    if (results[i].count > 0)
                        memcpy(items + n, results[i].items, results[i].count * sizeof(mr_kv_t));
                    n += results[i].count;
                    free(results[i].items);
                    mr_kv_list_init(&results[i]);
                }
                job->output.items = items;
                job->output.count = total;
                job->output.capacity = total;
                mr_kv_list_sort(&job->output);   /* same order for any thread count */
            }
        }
    }

    mr_free_map_table(results, r);
    free(group_counts);
    return err;
}
