/* test_threads.c - the multithreading: results, real parallelism, thread-safe
 * use of ctx, many repeated runs (also run this under ThreadSanitizer). */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mapreduce.h"
#include "test_common.h"
#include "test_util.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- a ctx shared by all threads, protected by its own mutex ------- */
typedef struct {
    pthread_mutex_t lock;
    int             calls;
    pthread_t       ids[256];
    int             num_ids;
    long            sleep_ms;
} shared_t;

static void note_thread(shared_t *sh)
{
    pthread_mutex_lock(&sh->lock);
    sh->calls++;
    int known = 0;
    for (int i = 0; i < sh->num_ids; i++)
        if (pthread_equal(sh->ids[i], pthread_self())) known = 1;
    if (!known && sh->num_ids < 256) sh->ids[sh->num_ids++] = pthread_self();
    pthread_mutex_unlock(&sh->lock);
}

/* map: records the calling thread, sleeps, then emits one pair per line */
static void slow_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    shared_t *sh = ctx;
    note_thread(sh);
    sleep_ms(sh->sleep_ms);
    wc_map(input, len, out, NULL);
}

static void slow_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                        size_t num_values, mr_emitter_t *out, void *ctx)
{
    shared_t *sh = ctx;
    note_thread(sh);
    sleep_ms(sh->sleep_ms);
    wc_reduce(key, key_len, values, num_values, out, NULL);
}

static void shared_init(shared_t *sh, long ms)
{
    memset(sh, 0, sizeof(*sh));
    pthread_mutex_init(&sh->lock, NULL);
    sh->sleep_ms = ms;
}

int main(void)
{
    mr_error_t err;
    char *baseline, *s;
    size_t len = 0;

    TEST_SECTION("the same answer for 1..8 mappers x 1..8 reducers");
    char *text = make_text(5000, &len);
    mr_job_t *job = run_wordcount(text, len, 1, 1, 200, MR_SPLIT_LINES, &err);
    baseline = output_string(job);
    CHECK("baseline (1 mapper, 1 reducer) has 50 words", mr_job_output(job)->count == 50);
    mr_job_destroy(job);
    int same = 1, runs = 0;
    for (size_t m = 1; m <= 8; m++) {
        for (size_t r = 1; r <= 8; r++) {
            job = run_wordcount(text, len, m, r, 200, MR_SPLIT_LINES, &err);
            s = job ? output_string(job) : NULL;
            if (s == NULL || strcmp(s, baseline) != 0) {
                same = 0;
                printf("    different for %zu mappers, %zu reducers\n", m, r);
            }
            free(s);
            mr_job_destroy(job);
            runs++;
        }
    }
    printf("  (%d combinations tried)\n", runs);
    CHECK("output is identical to the baseline every time", same);
    free(baseline);

    TEST_SECTION("how many threads are used");
    job = run_wordcount(text, len, 4, 3, 200, MR_SPLIT_LINES, &err);
    CHECK_EQ("one mapper per split: mappers = splits", mr_job_stats(job)->num_splits, mr_job_stats(job)->mappers_used);
    CHECK_EQ("3 reducers asked: 3 reducer threads", 3, mr_job_stats(job)->reducers_used);
    mr_job_destroy(job);
    job = run_wordcount(text, len, 8, 2, 1000000, MR_SPLIT_LINES, &err);
    CHECK_EQ("1 split only: just 1 mapper", 1, mr_job_stats(job)->mappers_used);
    mr_job_destroy(job);
    job = run_wordcount(text, len, 1, 1, 200, MR_SPLIT_LINES, &err);
    CHECK_EQ("1 reducer asked: 1 reducer thread", 1, mr_job_stats(job)->reducers_used);
    mr_job_destroy(job);
    free(text);

    TEST_SECTION("mappers really run at the same time");
    /* 8 lines = 8 splits; every map call sleeps 30 ms. One thread needs 240 ms;
     * 4 threads need about 60 ms. (Sleeping works on any number of cores.) */
    const char *eight = "a\nb\nc\nd\ne\nf\ng\nh\n";
    shared_t sh;
    shared_init(&sh, 30);
    job = mr_job_create();
    mr_job_set_map(job, slow_map, &sh);
    mr_job_set_reduce(job, wc_reduce, NULL);
    mr_job_set_split_mode(job, MR_SPLIT_LINES);
    mr_job_set_split_size(job, 1);
    mr_job_set_input(job, eight, strlen(eight));
    mr_job_set_mappers(job, 1);
    double t0 = now_ms();
    mr_job_run(job);
    double serial = now_ms() - t0;
    mr_job_set_mappers(job, 4);
    sh.calls = 0;
    sh.num_ids = 0;
    t0 = now_ms();
    mr_job_run(job);
    double parallel = now_ms() - t0;
    printf("  1 mapper: %.0f ms, 4 mappers: %.0f ms\n", serial, parallel);
    CHECK("1 mapper takes about 8 x 30 ms", serial >= 200);
    CHECK("4 mappers are at least 2x faster", parallel < serial / 2);
    CHECK_EQ("the map function was called once per split", 8, sh.calls);
    CHECK("at least 2 different threads ran map calls", sh.num_ids >= 2);
    mr_job_destroy(job);

    TEST_SECTION("reducers really run at the same time");
    char keys[16 * 4 + 1];
    for (int i = 0; i < 16; i++) sprintf(keys + i * 4, "k%02d ", i);
    shared_init(&sh, 20);
    job = mr_job_create();
    mr_job_set_map(job, wc_map, NULL);
    mr_job_set_reduce(job, slow_reduce, &sh);
    mr_job_set_input(job, keys, 16 * 4);
    mr_job_set_reducers(job, 1);
    t0 = now_ms();
    mr_job_run(job);
    serial = now_ms() - t0;
    mr_job_set_reducers(job, 4);
    sh.calls = 0;
    sh.num_ids = 0;
    t0 = now_ms();
    mr_job_run(job);
    parallel = now_ms() - t0;
    printf("  1 reducer: %.0f ms, 4 reducers: %.0f ms\n", serial, parallel);
    CHECK("1 reducer takes about 16 x 20 ms", serial >= 280);
    CHECK("4 reducers are clearly faster", parallel < serial * 0.75);
    CHECK_EQ("reduce was called once per key", 16, sh.calls);
    CHECK("at least 2 different threads ran reduce calls", sh.num_ids >= 2);
    mr_job_destroy(job);

    TEST_SECTION("number of mappers = input size / split size (one mapper per split)");
    {
        char data[1001];
        memset(data, 'a', sizeof(data) - 1);
        for (int i = 9; i < 1000; i += 10) data[i] = ' ';       /* words of 9 letters */
        data[1000] = '\0';
        size_t sizes[]    = { 100, 100, 300, 1000, 5000 };
        size_t lengths[]  = { 1000, 1001, 1000, 1000, 1000 };
        size_t expected[] = {   10,   11,    4,    1,    1 };    /* ceil(length / size) */
        for (int i = 0; i < 5; i++) {
            mr_job_t *j = mr_job_create();
            mr_job_set_map(j, wc_map, NULL);
            mr_job_set_reduce(j, wc_reduce, NULL);
            mr_job_set_split_mode(j, MR_SPLIT_BYTES);
            mr_job_set_split_size(j, sizes[i]);
            mr_job_set_input(j, data, lengths[i]);
            mr_error_t e = mr_job_run(j);
            CHECK_EQ("job runs", MR_OK, e);
            char label[80];
            sprintf(label, "input %zu bytes / split size %zu = %zu mappers", lengths[i], sizes[i], expected[i]);
            CHECK_EQ(label, expected[i], mr_job_stats(j)->mappers_used);
            mr_job_destroy(j);
        }
    }

    TEST_SECTION("the limit of 64 workers");
    text = make_text(2000, &len);
    job = run_wordcount(text, len, 64, 64, 100, MR_SPLIT_LINES, &err);
    CHECK_EQ("64 mappers and 64 reducers work", MR_OK, err);
    CHECK_EQ("one mapper per split (more splits than threads)", mr_job_stats(job)->num_splits, mr_job_stats(job)->mappers_used);
    mr_job_destroy(job);
    job = mr_job_create();
    CHECK_EQ("65 mappers are rejected", MR_ERR_INVALID, mr_job_set_mappers(job, 65));
    CHECK_EQ("65 reducers are rejected", MR_ERR_INVALID, mr_job_set_reducers(job, 65));
    mr_job_destroy(job);

    TEST_SECTION("stress: 300 jobs, different thread counts");
    int stress_ok = 1;
    baseline = NULL;
    job = run_wordcount(text, len, 1, 1, 100, MR_SPLIT_LINES, &err);
    baseline = output_string(job);
    mr_job_destroy(job);
    for (int i = 0; i < 300; i++) {
        size_t m = (size_t)(i % 7) + 1, r = (size_t)(i % 5) + 1;
        job = run_wordcount(text, len, m, r, (size_t)(50 + (i % 4) * 100), MR_SPLIT_LINES, &err);
        s = job ? output_string(job) : NULL;
        if (s == NULL || strcmp(s, baseline) != 0) stress_ok = 0;
        free(s);
        mr_job_destroy(job);
    }
    CHECK("all 300 answers are correct", stress_ok);
    free(baseline);
    free(text);

    return TEST_FINISH();
}
