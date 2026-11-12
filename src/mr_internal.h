/* mr_internal.h - private declarations shared by the library files.
 * Users of the library never include this file. */
#ifndef MR_INTERNAL_H
#define MR_INTERNAL_H

#include "mapreduce.h"

/* The job object (opaque to users). */
struct mr_job {
    mr_map_fn      map_fn;
    void          *map_ctx;
    mr_reduce_fn   reduce_fn;
    void          *reduce_ctx;

    size_t         num_mappers;
    size_t         num_reducers;
    size_t         split_size;
    mr_split_mode_t split_mode;

    const char    *input;        
    size_t         input_len;
    char          *owned_input;  
    int            has_input;

    mr_kv_list_t   output;      
    mr_job_stats_t stats;
    int            has_run;
};

typedef mr_error_t (*mr_task_fn)(size_t task_index, size_t worker_index,
                                 void *arg);

mr_error_t mr_run_parallel(size_t num_tasks, size_t num_workers,
                           mr_task_fn fn, void *arg, size_t *workers_used);


mr_error_t mr_map_phase(const struct mr_job *job, const mr_split_list_t *splits,
                        mr_kv_list_t **out_table, size_t *pairs_emitted,
                        size_t *mappers_used);
void mr_free_map_table(mr_kv_list_t *table, size_t num_lists);


mr_error_t mr_shuffle_phase(const struct mr_job *job, mr_kv_list_t *map_table,
                            size_t num_splits, mr_kv_list_t **out_partitions);


mr_error_t mr_reduce_phase(struct mr_job *job, mr_kv_list_t *partitions,
                           size_t *groups, size_t *reducers_used);

#endif /* MR_INTERNAL_H */
