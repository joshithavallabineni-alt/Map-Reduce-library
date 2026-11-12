/* mr_pool.c - runs a list of tasks on several POSIX threads.
 *
 * How the work is shared: there is ONE counter "next" (the number of the next
 * task nobody has started). A worker thread repeatedly
 *     lock the mutex  -> take the number, add 1 to the counter -> unlock
 *     run that task
 * until no tasks are left. So a thread that finishes early simply takes more
 * work (dynamic load balancing), and the mutex is held only for a moment.
 *
 * The tasks themselves do not need locks: every task writes only into its own
 * slot of a table, and the main thread reads the table only after pthread_join
 * (join is the synchronisation point that makes those writes visible). */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>

#include "mr_internal.h"

typedef struct {
    size_t          num_tasks;
    size_t          next;          /* protected by lock */
    mr_error_t      first_error;   /* protected by lock */
    mr_task_fn      fn;
    void           *arg;
    pthread_mutex_t lock;
} pool_t;

typedef struct {
    pool_t *pool;
    size_t  worker_index;
} worker_arg_t;

static void *worker_main(void *p)
{
    worker_arg_t *w = p;
    pool_t *pool = w->pool;

    for (;;) {
        pthread_mutex_lock(&pool->lock);
        if (pool->first_error != MR_OK || pool->next >= pool->num_tasks) {
            pthread_mutex_unlock(&pool->lock);
            break;                             /* nothing left (or stop) */
        }
        size_t task = pool->next++;
        pthread_mutex_unlock(&pool->lock);

        mr_error_t err = pool->fn(task, w->worker_index, pool->arg);
        if (err != MR_OK) {
            pthread_mutex_lock(&pool->lock);
            if (pool->first_error == MR_OK) pool->first_error = err;
            pthread_mutex_unlock(&pool->lock);
        }
    }
    return NULL;
}

mr_error_t mr_run_parallel(size_t num_tasks, size_t num_workers,
                           mr_task_fn fn, void *arg, size_t *workers_used)
{
    if (workers_used != NULL) *workers_used = 0;
    if (fn == NULL) return MR_ERR_NULL;
    if (num_workers == 0 || num_workers > MR_MAX_WORKERS) return MR_ERR_INVALID;
    if (num_tasks == 0) return MR_OK;
    if (num_workers > num_tasks) num_workers = num_tasks;   /* no idle threads */

    pool_t pool;
    pool.num_tasks = num_tasks;
    pool.next = 0;
    pool.first_error = MR_OK;
    pool.fn = fn;
    pool.arg = arg;
    if (pthread_mutex_init(&pool.lock, NULL) != 0) return MR_ERR_THREAD;

    worker_arg_t args[MR_MAX_WORKERS];
    pthread_t threads[MR_MAX_WORKERS];
    size_t started = 0;

    if (num_workers > 1) {
        for (size_t i = 0; i < num_workers; i++) {
            args[i].pool = &pool;
            args[i].worker_index = i;
            if (pthread_create(&threads[i], NULL, worker_main, &args[i]) != 0) break;
            started++;
        }
    }

    if (started == 0) {
        /* One worker was asked for (or no thread could be created): run the
         * same loop in the calling thread. */
        args[0].pool = &pool;
        args[0].worker_index = 0;
        worker_main(&args[0]);
        started = 1;
    } else {
        for (size_t i = 0; i < started; i++) pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&pool.lock);
    if (workers_used != NULL) *workers_used = started;
    return pool.first_error;
}
