#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mr_internal.h"

#define DEFAULT_SPLIT_SIZE 65536

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

mr_job_t *mr_job_create(void)
{
    mr_job_t *job = calloc(1, sizeof(mr_job_t));
    if (job == NULL) return NULL;
    job->num_mappers = 0;          
    job->num_reducers = 1;
    job->split_size = DEFAULT_SPLIT_SIZE;
    job->split_mode = MR_SPLIT_LINES;
    mr_kv_list_init(&job->output);
    return job;
}

void mr_job_destroy(mr_job_t *job)
{
    if (job == NULL) return;
    free(job->owned_input);
    mr_kv_list_free(&job->output);
    free(job);
}

mr_error_t mr_job_set_map(mr_job_t *job, mr_map_fn fn, void *ctx)
{
    if (job == NULL || fn == NULL) return MR_ERR_NULL;
    job->map_fn = fn;
    job->map_ctx = ctx;
    return MR_OK;
}

mr_error_t mr_job_set_reduce(mr_job_t *job, mr_reduce_fn fn, void *ctx)
{
    if (job == NULL || fn == NULL) return MR_ERR_NULL;
    job->reduce_fn = fn;
    job->reduce_ctx = ctx;
    return MR_OK;
}

mr_error_t mr_job_set_mappers(mr_job_t *job, size_t n)
{
    if (job == NULL) return MR_ERR_NULL;
    if (n > MR_MAX_WORKERS) return MR_ERR_INVALID;     /* 0 is allowed: automatic */
    job->num_mappers = n;
    return MR_OK;
}

mr_error_t mr_job_set_reducers(mr_job_t *job, size_t n)
{
    if (job == NULL) return MR_ERR_NULL;
    if (n == 0 || n > MR_MAX_WORKERS) return MR_ERR_INVALID;
    job->num_reducers = n;
    return MR_OK;
}

mr_error_t mr_job_set_split_size(mr_job_t *job, size_t bytes)
{
    if (job == NULL) return MR_ERR_NULL;
    if (bytes == 0) return MR_ERR_INVALID;
    job->split_size = bytes;
    return MR_OK;
}

mr_error_t mr_job_set_split_mode(mr_job_t *job, mr_split_mode_t mode)
{
    if (job == NULL) return MR_ERR_NULL;
    if (mode != MR_SPLIT_BYTES && mode != MR_SPLIT_WORDS && mode != MR_SPLIT_LINES)
        return MR_ERR_INVALID;
    job->split_mode = mode;
    return MR_OK;
}

mr_error_t mr_job_set_input(mr_job_t *job, const char *data, size_t len)
{
    if (job == NULL) return MR_ERR_NULL;
    if (data == NULL && len > 0) return MR_ERR_NULL;
    free(job->owned_input);          /* forget an earlier file input */
    job->owned_input = NULL;
    job->input = data;
    job->input_len = len;
    job->has_input = 1;
    return MR_OK;
}

mr_error_t mr_job_set_input_file(mr_job_t *job, const char *path)
{
    if (job == NULL || path == NULL) return MR_ERR_NULL;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return MR_ERR_IO;

    size_t capacity = 65536, len = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        fclose(f);
        return MR_ERR_NO_MEMORY;
    }
    for (;;) {
        if (len == capacity) {
            char *bigger = realloc(buffer, capacity * 2);
            if (bigger == NULL) {
                free(buffer);
                fclose(f);
                return MR_ERR_NO_MEMORY;
            }
            buffer = bigger;
            capacity *= 2;
        }
        size_t got = fread(buffer + len, 1, capacity - len, f);
        len += got;
        if (got == 0) break;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        free(buffer);
        return MR_ERR_IO;
    }

    free(job->owned_input);
    job->owned_input = buffer;
    job->input = buffer;
    job->input_len = len;
    job->has_input = 1;
    return MR_OK;
}

mr_error_t mr_job_run(mr_job_t *job)
{
    if (job == NULL) return MR_ERR_NULL;
    if (job->map_fn == NULL || job->reduce_fn == NULL || !job->has_input) return MR_ERR_STATE;

    /* Forget the result of an earlier run. */
    mr_kv_list_free(&job->output);
    memset(&job->stats, 0, sizeof(job->stats));
    job->has_run = 0;

    mr_split_list_t splits;
    mr_kv_list_t *map_table = NULL;
    mr_kv_list_t *partitions = NULL;
    mr_job_stats_t st;
    memset(&st, 0, sizeof(st));
    mr_error_t err;
    double t0 = now_ms(), t1;

    /* 1. split */
    err = mr_split_input(job->input, job->input_len, job->split_size,
                         job->split_mode, &splits);
    if (err != MR_OK) return err;
    st.num_splits = splits.count;
    t1 = now_ms();
    st.split_ms = t1 - t0;

    /* 2. map */
    err = mr_map_phase(job, &splits, &map_table, &st.intermediate_pairs, &st.mappers_used);
    if (err != MR_OK) goto fail;
    t0 = now_ms();
    st.map_ms = t0 - t1;

    /* 3. shuffle (moves the pairs out of map_table, then sorts) */
    err = mr_shuffle_phase(job, map_table, splits.count, &partitions);
    mr_free_map_table(map_table, splits.count * job->num_reducers);
    map_table = NULL;
    if (err != MR_OK) goto fail;
    t1 = now_ms();
    st.shuffle_ms = t1 - t0;

    /* 4. reduce */
    err = mr_reduce_phase(job, partitions, &st.groups, &st.reducers_used);
    if (err != MR_OK) goto fail;
    t0 = now_ms();
    st.reduce_ms = t0 - t1;

    st.output_pairs = job->output.count;
    st.total_ms = st.split_ms + st.map_ms + st.shuffle_ms + st.reduce_ms;
    job->stats = st;
    job->has_run = 1;

    mr_free_map_table(partitions, job->num_reducers);
    mr_split_list_free(&splits);
    return MR_OK;

fail:
    mr_free_map_table(map_table, splits.count * job->num_reducers);
    mr_free_map_table(partitions, job->num_reducers);
    mr_split_list_free(&splits);
    mr_kv_list_free(&job->output);
    return err;
}

const mr_kv_list_t *mr_job_output(const mr_job_t *job)
{
    static const mr_kv_list_t empty = { NULL, 0, 0 };
    if (job == NULL) return &empty;
    return &job->output;
}

const mr_job_stats_t *mr_job_stats(const mr_job_t *job)
{
    static const mr_job_stats_t zero;
    if (job == NULL) return &zero;
    return &job->stats;
}

mr_error_t mr_job_write_output(const mr_job_t *job, const char *path)
{
    if (job == NULL || path == NULL) return MR_ERR_NULL;
    if (!job->has_run) return MR_ERR_STATE;

    FILE *f = (strcmp(path, "-") == 0) ? stdout : fopen(path, "wb");
    if (f == NULL) return MR_ERR_IO;

    int ok = 1;
    for (size_t i = 0; i < job->output.count && ok; i++) {
        const mr_kv_t *p = &job->output.items[i];
        if (fwrite(p->key, 1, p->key_len, f) != p->key_len) ok = 0;
        if (ok && fputc('\t', f) == EOF) ok = 0;
        if (ok && fwrite(p->value, 1, p->value_len, f) != p->value_len) ok = 0;
        if (ok && fputc('\n', f) == EOF) ok = 0;
    }
    if (f == stdout) {
        if (fflush(f) != 0) ok = 0;
    } else if (fclose(f) != 0) {
        ok = 0;
    }
    return ok ? MR_OK : MR_ERR_IO;
}
