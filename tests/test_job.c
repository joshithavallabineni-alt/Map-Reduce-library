/* test_job.c - the whole library: map -> shuffle -> reduce -> output.
 * Uses small inputs whose correct answer can be checked by hand. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mapreduce.h"
#include "test_common.h"
#include "test_util.h"

static const char TEXT[] = "the cat sat dog the mat sat the cow dog the cat sat dog the cow ";
static const char EXPECTED[] = "cat=2;cow=2;dog=3;mat=1;sat=3;the=5;";

/* ---- special map/reduce functions used below ---------------------- */

/* map: emits (prefix + word, "1"); the prefix comes through ctx */
static void prefix_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    const char *prefix = ctx;
    size_t i = 0;
    while (i < len) {
        while (i < len && input[i] == ' ') i++;
        size_t s = i;
        while (i < len && input[i] != ' ') i++;
        if (i > s) {
            char key[64];
            int n = snprintf(key, sizeof(key), "%s%.*s", prefix, (int)(i - s), input + s);
            mr_emit(out, key, (size_t)n, "1", 1);
        }
    }
}

/* reduce: only keeps words that occur at least 3 times */
static void frequent_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                            size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx; (void)values;
    if (num_values >= 3) {
        char text[16];
        int n = snprintf(text, sizeof(text), "%zu", num_values);
        mr_emit(out, key, key_len, text, (size_t)n);
    }
}

/* reduce: emits TWO pairs per key */
static void double_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                          size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx; (void)values;
    char text[16];
    int n = snprintf(text, sizeof(text), "%zu", num_values);
    mr_emit(out, key, key_len, text, (size_t)n);
    mr_emit(out, "total", 5, text, (size_t)n);
}

/* reduce: checks that the values arrive sorted (ctx is an int flag) */
static void order_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                         size_t num_values, mr_emitter_t *out, void *ctx)
{
    int *sorted = ctx;
    for (size_t i = 1; i < num_values; i++)
        if (mr_kv_compare(&values[i - 1], &values[i]) > 0) *sorted = 0;
    mr_emit(out, key, key_len, "ok", 2);
}

/* map: emits a key with an embedded '\0' byte and an empty value */
static void binary_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)input; (void)len; (void)ctx;
    mr_emit(out, "a\0b", 3, "", 0);
    mr_emit(out, "a\0b", 3, "", 0);
    mr_emit(out, "a", 1, "", 0);
}

static void count_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                         size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx; (void)values;
    char text[16];
    int n = snprintf(text, sizeof(text), "%zu", num_values);
    mr_emit(out, key, key_len, text, (size_t)n);
}

static mr_job_t *make_job(mr_map_fn m, void *mctx, mr_reduce_fn r, void *rctx,
                          const char *text, size_t len)
{
    mr_job_t *job = mr_job_create();
    mr_job_set_map(job, m, mctx);
    mr_job_set_reduce(job, r, rctx);
    mr_job_set_input(job, text, len);
    return job;
}

int main(void)
{
    mr_error_t err;
    char *s;

    TEST_SECTION("word count with known answer (1 mapper, 1 reducer)");
    mr_job_t *job = run_wordcount(TEXT, strlen(TEXT), 1, 1, 16, MR_SPLIT_WORDS, &err);
    CHECK_EQ("job status", MR_OK, err);
    s = output_string(job);
    CHECK("output is cat=2 cow=2 dog=3 mat=1 sat=3 the=5", s != NULL && strcmp(s, EXPECTED) == 0);
    free(s);
    const mr_job_stats_t *st = mr_job_stats(job);
    CHECK_EQ("4 splits", 4, st->num_splits);
    CHECK_EQ("16 intermediate pairs", 16, st->intermediate_pairs);
    CHECK_EQ("6 groups (different words)", 6, st->groups);
    CHECK_EQ("6 output pairs", 6, st->output_pairs);
    CHECK_EQ("mr_job_output count agrees", 6, mr_job_output(job)->count);
    CHECK("times are not negative", st->map_ms >= 0 && st->shuffle_ms >= 0 && st->reduce_ms >= 0 && st->total_ms >= 0);
    mr_job_destroy(job);

    TEST_SECTION("the answer does not depend on the number of workers or the split size");
    size_t split_sizes[] = { 1, 8, 16, 33, 64, 4096 };
    int same = 1, runs = 0;
    for (int z = 0; z < 6; z++) {
        for (size_t m = 1; m <= 4; m++) {
            for (size_t r = 1; r <= 4; r++) {
                job = run_wordcount(TEXT, strlen(TEXT), m, r, split_sizes[z], MR_SPLIT_WORDS, &err);
                s = job ? output_string(job) : NULL;
                if (s == NULL || strcmp(s, EXPECTED) != 0) {
                    same = 0;
                    printf("    different for split size %zu, %zu mappers, %zu reducers\n", split_sizes[z], m, r);
                }
                free(s);
                mr_job_destroy(job);
                runs++;
            }
        }
    }
    printf("  (%d combinations tried)\n", runs);
    CHECK("always cat=2;cow=2;dog=3;mat=1;sat=3;the=5", same);

    TEST_SECTION("defaults: only functions and input are set");
    job = make_job(wc_map, NULL, wc_reduce, NULL, TEXT, strlen(TEXT));
    CHECK_EQ("run with the defaults", MR_OK, mr_job_run(job));
    s = output_string(job);
    CHECK("correct answer", s != NULL && strcmp(s, EXPECTED) == 0);
    free(s);

    TEST_SECTION("running the same job again");
    mr_job_set_mappers(job, 3);
    mr_job_set_reducers(job, 2);
    CHECK_EQ("second run", MR_OK, mr_job_run(job));
    s = output_string(job);
    CHECK("same answer, not doubled", s != NULL && strcmp(s, EXPECTED) == 0);
    free(s);
    mr_job_set_input(job, "x y x", 5);
    CHECK_EQ("run with a new input", MR_OK, mr_job_run(job));
    s = output_string(job);
    CHECK("answer for the new input only", s != NULL && strcmp(s, "x=2;y=1;") == 0);
    free(s);
    mr_job_destroy(job);

    TEST_SECTION("before the first run");
    job = mr_job_create();
    CHECK_EQ("output is empty", 0, mr_job_output(job)->count);
    CHECK_EQ("statistics are zero", 0, mr_job_stats(job)->num_splits);
    mr_job_destroy(job);

    TEST_SECTION("ctx pointer reaches the map function");
    job = make_job(prefix_map, "w_", wc_reduce, NULL, TEXT, strlen(TEXT));
    mr_job_set_mappers(job, 2);
    mr_job_set_split_mode(job, MR_SPLIT_WORDS);
    mr_job_set_split_size(job, 16);
    CHECK_EQ("run", MR_OK, mr_job_run(job));
    CHECK_EQ("w_the counted 5 times", 5, output_value(job, "w_the"));
    CHECK_EQ("\"the\" without prefix does not exist", -1, output_value(job, "the"));
    mr_job_destroy(job);

    TEST_SECTION("reduce may emit nothing, or several pairs");
    job = make_job(wc_map, NULL, frequent_reduce, NULL, TEXT, strlen(TEXT));
    mr_job_set_split_mode(job, MR_SPLIT_WORDS);
    mr_job_run(job);
    s = output_string(job);
    CHECK("only dog=3, sat=3, the=5 remain", s != NULL && strcmp(s, "dog=3;sat=3;the=5;") == 0);
    free(s);
    mr_job_destroy(job);
    job = make_job(wc_map, NULL, double_reduce, NULL, "a b a", 5);
    mr_job_set_reducers(job, 2);
    mr_job_run(job);
    CHECK_EQ("2 keys x 2 pairs = 4 output pairs", 4, mr_job_output(job)->count);
    mr_job_destroy(job);

    TEST_SECTION("the values of a key arrive sorted");
    int sorted = 1;
    job = make_job(prefix_map, "", order_reduce, &sorted, TEXT, strlen(TEXT));
    mr_job_set_split_mode(job, MR_SPLIT_WORDS);
    mr_job_set_split_size(job, 8);
    mr_job_run(job);
    CHECK("sorted by value in every group", sorted);
    mr_job_destroy(job);

    TEST_SECTION("keys with '\\0', empty values");
    job = make_job(binary_map, NULL, count_reduce, NULL, "x", 1);
    CHECK_EQ("run", MR_OK, mr_job_run(job));
    CHECK_EQ("2 different keys (\"a\\0b\" and \"a\")", 2, mr_job_output(job)->count);
    CHECK("\"a\" sorts before \"a\\0b\"", mr_job_output(job)->items[0].key_len == 1 &&
                                         mr_job_output(job)->items[1].key_len == 3);
    CHECK("\"a\\0b\" was counted twice", strcmp(mr_job_output(job)->items[1].value, "2") == 0);
    mr_job_destroy(job);

    TEST_SECTION("empty and unusual input");
    job = make_job(wc_map, NULL, wc_reduce, NULL, "", 0);
    CHECK_EQ("empty input runs", MR_OK, mr_job_run(job));
    CHECK_EQ("no output", 0, mr_job_output(job)->count);
    CHECK_EQ("no splits", 0, mr_job_stats(job)->num_splits);
    mr_job_set_input(job, NULL, 0);
    CHECK_EQ("(NULL, 0) is an empty input", MR_OK, mr_job_run(job));
    mr_job_set_input(job, "  \n \t \n", 7);
    CHECK_EQ("only white space", MR_OK, mr_job_run(job));
    CHECK_EQ("no output", 0, mr_job_output(job)->count);
    mr_job_destroy(job);

    TEST_SECTION("input from a file");
    char path[] = "/tmp/mr_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        CHECK("could not create a temporary file", 0);
    } else {
        if (write(fd, TEXT, strlen(TEXT)) < 0) { /* ignore: checked below */ }
        close(fd);
        job = mr_job_create();
        mr_job_set_map(job, wc_map, NULL);
        mr_job_set_reduce(job, wc_reduce, NULL);
        mr_job_set_split_mode(job, MR_SPLIT_WORDS);
        mr_job_set_mappers(job, 2);
        CHECK_EQ("read the file", MR_OK, mr_job_set_input_file(job, path));
        CHECK_EQ("run", MR_OK, mr_job_run(job));
        s = output_string(job);
        CHECK("same answer as the in-memory input", s != NULL && strcmp(s, EXPECTED) == 0);
        free(s);

        TEST_SECTION("writing the output to a file");
        char out_path[] = "/tmp/mr_out_XXXXXX";
        int ofd = mkstemp(out_path);
        close(ofd);
        CHECK_EQ("write", MR_OK, mr_job_write_output(job, out_path));
        FILE *f = fopen(out_path, "rb");
        char content[256] = { 0 };
        size_t got = f ? fread(content, 1, sizeof(content) - 1, f) : 0;
        if (f) fclose(f);
        CHECK("lines are key<TAB>value",
              got > 0 && strncmp(content, "cat\t2\ncow\t2\ndog\t3\nmat\t1\nsat\t3\nthe\t5\n", got) == 0 && got == 36);
        remove(out_path);
        mr_job_destroy(job);
        remove(path);
    }

    TEST_SECTION("the output is sorted");
    job = run_wordcount(TEXT, strlen(TEXT), 3, 3, 8, MR_SPLIT_WORDS, &err);
    int in_order = 1;
    const mr_kv_list_t *out = mr_job_output(job);
    for (size_t i = 1; i < out->count; i++)
        if (mr_kv_compare(&out->items[i - 1], &out->items[i]) > 0) in_order = 0;
    CHECK("keys are in increasing order (across all reducers)", in_order);
    mr_job_destroy(job);

    return TEST_FINISH();
}
