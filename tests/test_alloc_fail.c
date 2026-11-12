/* test_alloc_fail.c - what happens when malloc/calloc/realloc FAIL?
 *
 * The Makefile links this test with  -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc
 * so that every allocation made by the library goes through the functions
 * below first. We let the n-th allocation fail, for n = 1, 2, 3, ... until the
 * job finally succeeds. For EVERY n the job must either succeed or return
 * MR_ERR_NO_MEMORY (never crash). Run under valgrind or AddressSanitizer to
 * see that nothing leaks on any of these failure paths. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapreduce.h"
#include "test_common.h"
#include "test_util.h"

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);

static atomic_long fail_at = -1;      /* fail the allocation with this number (-1: never) */
static atomic_long allocations = 0;
static atomic_int  failed_once = 0;

static int should_fail(void)
{
    long n = atomic_fetch_add(&allocations, 1);
    if (n == atomic_load(&fail_at)) {
        atomic_store(&failed_once, 1);
        return 1;
    }
    return 0;
}

void *__wrap_malloc(size_t size)            { return should_fail() ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t n, size_t size)  { return should_fail() ? NULL : __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t size) { return should_fail() ? NULL : __real_realloc(p, size); }

static const char TEXT[] = "the cat sat dog the mat sat the cow dog the cat sat dog the cow ";

/* Runs one job; returns its status. */
static mr_error_t one_job(size_t mappers, size_t reducers, int *correct)
{
    mr_error_t err;
    mr_job_t *job = run_wordcount(TEXT, strlen(TEXT), mappers, reducers, 16, MR_SPLIT_WORDS, &err);
    *correct = 1;
    if (err == MR_OK) {
        const mr_kv_list_t *out = mr_job_output(job);
        unsigned long total = 0;
        for (size_t i = 0; i < out->count; i++) total += strtoul(out->items[i].value, NULL, 10);
        *correct = (out->count == 6 && total == 16);
    }
    mr_job_destroy(job);
    return err;
}

int main(void)
{
    size_t configs[][2] = { { 1, 1 }, { 3, 2 } };
    for (int c = 0; c < 2; c++) {
        char title[96];
        sprintf(title, "let the n-th allocation fail: %zu mapper(s), %zu reducer(s)", configs[c][0], configs[c][1]);
        TEST_SECTION(title);

        int all_clean = 1, all_correct = 1;
        long n = 0, failures_seen = 0;
        for (;; n++) {
            atomic_store(&allocations, 0);
            atomic_store(&failed_once, 0);
            atomic_store(&fail_at, n);
            int correct;
            mr_error_t err = one_job(configs[c][0], configs[c][1], &correct);
            atomic_store(&fail_at, -1);
            if (!atomic_load(&failed_once)) break;         /* allocation n never happened: we are done */
            failures_seen++;
            if (err != MR_OK && err != MR_ERR_NO_MEMORY) all_clean = 0;
            if (err == MR_OK && !correct) all_correct = 0;  /* (a failure that was handled internally) */
        }
        printf("  (%ld different allocation failures tried)\n", failures_seen);
        CHECK("every failure gave MR_OK or MR_ERR_NO_MEMORY", all_clean);
        CHECK("a job that reports MR_OK has the right answer", all_correct);
        CHECK("at least 50 allocation points were exercised", failures_seen >= 50);
    }

    TEST_SECTION("the same without failures");
    int correct;
    CHECK_EQ("job status", MR_OK, one_job(3, 2, &correct));
    CHECK("correct answer", correct);

    return TEST_FINISH();
}
