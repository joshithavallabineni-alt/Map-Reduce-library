/* test_large.c - large inputs: millions of words, hundreds of thousands of
 * different keys, 100000 splits, very big keys and values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapreduce.h"
#include "test_common.h"
#include "test_util.h"

/* map: every line "key" -> (key, 1 MB value of 'v') for the first split only */
static void big_value_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)input; (void)len; (void)ctx;
    size_t n = 1u << 20;
    char *value = malloc(n);
    char *key = malloc(100000);
    if (value == NULL || key == NULL) {
        free(value);
        free(key);
        mr_emit_fail(out);
        return;
    }
    memset(value, 'v', n);
    memset(key, 'k', 100000);
    mr_emit(out, key, 100000, value, n);
    free(value);
    free(key);
}

static void length_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                          size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    char text[64];
    int n = snprintf(text, sizeof(text), "%zu,%zu", num_values, values[0].value_len);
    mr_emit(out, key, key_len < 5 ? key_len : 5, text, (size_t)n);
}

int main(void)
{
    mr_error_t err;
    size_t len = 0;

    TEST_SECTION("2,000,000 words, 4 mappers, 4 reducers");
    size_t words = 2000000;
    char *text = make_text(words, &len);
    mr_job_t *job = run_wordcount(text, len, 4, 4, 65536, MR_SPLIT_LINES, &err);
    CHECK_EQ("status", MR_OK, err);
    CHECK_EQ("50 different words", 50, mr_job_output(job)->count);
    unsigned long total = 0;
    const mr_kv_list_t *out = mr_job_output(job);
    for (size_t i = 0; i < out->count; i++) total += strtoul(out->items[i].value, NULL, 10);
    CHECK_EQ("the counts add up to 2,000,000", words, total);
    CHECK_EQ("2,000,000 intermediate pairs", words, mr_job_stats(job)->intermediate_pairs);

    /* The same word count computed with a plain loop over the formula. */
    unsigned long expected[50] = { 0 };
    for (size_t i = 0; i < words; i++) expected[(i * 7 + i / 3) % 50]++;
    int exact = 1;
    for (int id = 0; id < 50; id++) {
        char key[8];
        sprintf(key, "w%d", id);
        if (output_value(job, key) != (long)expected[id]) exact = 0;
    }
    CHECK("every one of the 50 counts is exactly right", exact);
    mr_job_destroy(job);
    free(text);

    TEST_SECTION("500,000 different keys");
    size_t keys = 500000;
    text = malloc(keys * 8 + 8);
    len = 0;
    for (size_t i = 0; i < keys; i++) len += (size_t)sprintf(text + len, "k%zu%c", i, (i % 10 == 9) ? '\n' : ' ');
    job = run_wordcount(text, len, 4, 4, 65536, MR_SPLIT_LINES, &err);
    CHECK_EQ("status", MR_OK, err);
    CHECK_EQ("500,000 output pairs", keys, mr_job_output(job)->count);
    CHECK_EQ("500,000 groups", keys, mr_job_stats(job)->groups);
    mr_job_destroy(job);
    free(text);

    TEST_SECTION("100,000 splits (one per line), 8 mappers, 8 reducers");
    size_t lines = 100000;
    text = malloc(lines * 8 + 8);
    len = 0;
    for (size_t i = 0; i < lines; i++) len += (size_t)sprintf(text + len, "a%zu\n", i % 10);
    job = run_wordcount(text, len, 8, 8, 1, MR_SPLIT_LINES, &err);
    CHECK_EQ("status", MR_OK, err);
    CHECK_EQ("100,000 splits", lines, mr_job_stats(job)->num_splits);
    CHECK_EQ("a3 occurs 10,000 times", 10000, output_value(job, "a3"));
    mr_job_destroy(job);
    free(text);

    TEST_SECTION("a 100 KB key and a 1 MB value");
    job = mr_job_create();
    mr_job_set_map(job, big_value_map, NULL);
    mr_job_set_reduce(job, length_reduce, NULL);
    mr_job_set_input(job, "x", 1);
    mr_job_set_mappers(job, 2);
    mr_job_set_reducers(job, 2);
    CHECK_EQ("status", MR_OK, mr_job_run(job));
    CHECK("1 group, value length 1048576", mr_job_output(job)->count == 1 &&
          strcmp(mr_job_output(job)->items[0].value, "1,1048576") == 0);
    mr_job_destroy(job);

    return TEST_FINISH();
}
