/* test_memory.c - memory management. Runs many complete jobs (successful and
 * failing) and destroys everything each time. By itself it only shows that
 * nothing crashes; the real check is a memory tool:
 *
 *     valgrind --leak-check=full ./tests/test_memory
 *     make asan            (AddressSanitizer + UBSan on all tests)
 *
 * A clean run reports no leaks and no invalid reads or writes. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mapreduce.h"
#include "test_common.h"
#include "test_util.h"

static const char TEXT[] = "the cat sat dog the mat sat the cow dog the cat sat dog the cow ";

static void fail_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    wc_map(input, len, out, NULL);
    if (len > 0 && memchr(input, 'm', len) != NULL) mr_emit_fail(out);   /* after emitting some pairs */
}

int main(void)
{
    mr_error_t err;

    TEST_SECTION("200 complete jobs, destroyed each time");
    int all_ok = 1;
    for (int i = 0; i < 200; i++) {
        mr_job_t *job = run_wordcount(TEXT, strlen(TEXT), (size_t)(i % 4) + 1, (size_t)(i % 3) + 1,
                                      (size_t)(8 + (i % 5) * 8), MR_SPLIT_WORDS, &err);
        if (job == NULL || mr_job_output(job)->count != 6) all_ok = 0;
        mr_job_destroy(job);
    }
    CHECK("all 200 jobs gave 6 words", all_ok);

    TEST_SECTION("100 failing jobs (pairs already emitted must be freed)");
    int all_failed = 1;
    for (int i = 0; i < 100; i++) {
        mr_job_t *job = mr_job_create();
        mr_job_set_map(job, fail_map, NULL);
        mr_job_set_reduce(job, wc_reduce, NULL);
        mr_job_set_mappers(job, (size_t)(i % 3) + 1);
        mr_job_set_reducers(job, (size_t)(i % 4) + 1);
        mr_job_set_split_mode(job, MR_SPLIT_WORDS);
        mr_job_set_split_size(job, 16);
        mr_job_set_input(job, TEXT, strlen(TEXT));
        if (mr_job_run(job) != MR_ERR_CALLBACK) all_failed = 0;
        mr_job_destroy(job);
    }
    CHECK("every failing job returned MR_ERR_CALLBACK", all_failed);

    TEST_SECTION("one job, run many times, input and settings changed");
    mr_job_t *job = mr_job_create();
    mr_job_set_map(job, wc_map, NULL);
    mr_job_set_reduce(job, wc_reduce, NULL);
    int reuse_ok = 1;
    for (int i = 0; i < 100; i++) {
        mr_job_set_mappers(job, (size_t)(i % 4) + 1);
        mr_job_set_reducers(job, (size_t)(i % 3) + 1);
        mr_job_set_split_mode(job, MR_SPLIT_WORDS);
        mr_job_set_split_size(job, 12);
        mr_job_set_input(job, TEXT, strlen(TEXT));
        if (mr_job_run(job) != MR_OK || mr_job_output(job)->count != 6) reuse_ok = 0;
    }
    CHECK("100 runs, always correct", reuse_ok);
    mr_job_destroy(job);

    TEST_SECTION("file input is replaced and freed");
    char path[] = "/tmp/mr_mem_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        if (write(fd, TEXT, strlen(TEXT)) < 0) { /* checked by the run below */ }
        close(fd);
        job = mr_job_create();
        mr_job_set_map(job, wc_map, NULL);
        mr_job_set_reduce(job, wc_reduce, NULL);
        int file_ok = 1;
        for (int i = 0; i < 20; i++) {
            if (mr_job_set_input_file(job, path) != MR_OK) file_ok = 0;   /* frees the earlier copy */
        }
        mr_job_set_input(job, TEXT, strlen(TEXT));                          /* ... and so does this */
        if (mr_job_run(job) != MR_OK) file_ok = 0;
        mr_job_set_input_file(job, path);
        if (mr_job_run(job) != MR_OK) file_ok = 0;
        CHECK("file input many times, then run", file_ok);
        mr_job_destroy(job);                                                /* frees the last copy */
        remove(path);
    }

    TEST_SECTION("destroying jobs in every state");
    job = mr_job_create();
    mr_job_destroy(job);
    job = mr_job_create();
    mr_job_set_input(job, TEXT, strlen(TEXT));
    mr_job_destroy(job);
    mr_job_destroy(NULL);
    CHECK("never run / only configured / NULL: no crash", 1);

    return TEST_FINISH();
}
