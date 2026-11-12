/* test_errors.c - bad arguments, wrong order of calls, failing user functions,
 * file problems. The library must return an error code and never crash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapreduce.h"
#include "test_common.h"
#include "test_util.h"

/* map: fails (mr_emit_fail) on a split that contains the letter 'x' */
static void fail_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        if (input[i] == 'x') {
            mr_emit_fail(out);
            return;
        }
    }
    wc_map(input, len, out, NULL);
}

static void null_key_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)input; (void)len; (void)ctx;
    mr_emit(out, NULL, 0, "1", 1);
}

static void null_value_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)input; (void)len; (void)ctx;
    mr_emit(out, "k", 1, NULL, 5);       /* NULL value with a length */
}

static void fail_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                        size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)key; (void)key_len; (void)values; (void)num_values; (void)ctx;
    mr_emit_fail(out);
}

static void late_error_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)input; (void)len; (void)ctx;
    mr_emit(out, NULL, 0, "1", 1);        /* error ... */
    mr_emit(out, "later", 5, "1", 1);     /* ... later emits must do nothing */
}

int main(void)
{
    mr_job_t *job;

    TEST_SECTION("NULL job in every function");
    CHECK_EQ("set_map", MR_ERR_NULL, mr_job_set_map(NULL, wc_map, NULL));
    CHECK_EQ("set_reduce", MR_ERR_NULL, mr_job_set_reduce(NULL, wc_reduce, NULL));
    CHECK_EQ("set_mappers", MR_ERR_NULL, mr_job_set_mappers(NULL, 2));
    CHECK_EQ("set_reducers", MR_ERR_NULL, mr_job_set_reducers(NULL, 2));
    CHECK_EQ("set_split_size", MR_ERR_NULL, mr_job_set_split_size(NULL, 10));
    CHECK_EQ("set_split_mode", MR_ERR_NULL, mr_job_set_split_mode(NULL, MR_SPLIT_LINES));
    CHECK_EQ("set_input", MR_ERR_NULL, mr_job_set_input(NULL, "x", 1));
    CHECK_EQ("set_input_file", MR_ERR_NULL, mr_job_set_input_file(NULL, "/tmp/x"));
    CHECK_EQ("run", MR_ERR_NULL, mr_job_run(NULL));
    CHECK_EQ("write_output", MR_ERR_NULL, mr_job_write_output(NULL, "-"));
    CHECK_EQ("output of NULL is an empty list", 0, mr_job_output(NULL)->count);
    CHECK_EQ("stats of NULL are zero", 0, mr_job_stats(NULL)->total_ms > 0);
    mr_job_destroy(NULL);
    CHECK("mr_job_destroy(NULL) is safe", 1);

    TEST_SECTION("invalid values");
    job = mr_job_create();
    CHECK_EQ("NULL map function", MR_ERR_NULL, mr_job_set_map(job, NULL, NULL));
    CHECK_EQ("NULL reduce function", MR_ERR_NULL, mr_job_set_reduce(job, NULL, NULL));
    CHECK_EQ("0 mappers = automatic, allowed", MR_OK, mr_job_set_mappers(job, 0));
    CHECK_EQ("0 reducers", MR_ERR_INVALID, mr_job_set_reducers(job, 0));
    CHECK_EQ("too many mappers", MR_ERR_INVALID, mr_job_set_mappers(job, MR_MAX_WORKERS + 1));
    CHECK_EQ("huge number of reducers", MR_ERR_INVALID, mr_job_set_reducers(job, (size_t)-1));
    CHECK_EQ("split size 0", MR_ERR_INVALID, mr_job_set_split_size(job, 0));
    CHECK_EQ("unknown split mode", MR_ERR_INVALID, mr_job_set_split_mode(job, (mr_split_mode_t)9));
    CHECK_EQ("NULL input with length > 0", MR_ERR_NULL, mr_job_set_input(job, NULL, 4));
    CHECK_EQ("NULL file name", MR_ERR_NULL, mr_job_set_input_file(job, NULL));
    CHECK_EQ("1 mapper is fine", MR_OK, mr_job_set_mappers(job, 1));
    CHECK_EQ("64 reducers are fine", MR_OK, mr_job_set_reducers(job, MR_MAX_WORKERS));

    TEST_SECTION("running before the job is ready");
    mr_job_destroy(job);
    job = mr_job_create();
    CHECK_EQ("nothing set", MR_ERR_STATE, mr_job_run(job));
    mr_job_set_map(job, wc_map, NULL);
    CHECK_EQ("only map set", MR_ERR_STATE, mr_job_run(job));
    mr_job_set_reduce(job, wc_reduce, NULL);
    CHECK_EQ("no input", MR_ERR_STATE, mr_job_run(job));
    CHECK_EQ("writing output before a run", MR_ERR_STATE, mr_job_write_output(job, "/tmp/mr_never"));
    mr_job_set_input(job, "a b", 3);
    CHECK_EQ("now everything is set", MR_OK, mr_job_run(job));
    mr_job_destroy(job);

    TEST_SECTION("a map function that fails");
    job = mr_job_create();
    mr_job_set_map(job, fail_map, NULL);
    mr_job_set_reduce(job, wc_reduce, NULL);
    mr_job_set_mappers(job, 3);
    mr_job_set_split_mode(job, MR_SPLIT_LINES);
    mr_job_set_split_size(job, 1);
    mr_job_set_input(job, "a b\nc d\nx y\ne f\ng h\n", 20);
    CHECK_EQ("the job returns MR_ERR_CALLBACK", MR_ERR_CALLBACK, mr_job_run(job));
    CHECK_EQ("the output is empty", 0, mr_job_output(job)->count);
    CHECK_EQ("writing the (failed) output is refused", MR_ERR_STATE, mr_job_write_output(job, "/tmp/mr_never"));
    mr_job_set_input(job, "a b\nc d\ne f\n", 12);
    CHECK_EQ("the same job works with good input", MR_OK, mr_job_run(job));
    CHECK_EQ("6 words in the output", 6, mr_job_output(job)->count);
    mr_job_destroy(job);

    TEST_SECTION("map emits invalid pairs");
    job = mr_job_create();
    mr_job_set_map(job, null_key_map, NULL);
    mr_job_set_reduce(job, wc_reduce, NULL);
    mr_job_set_input(job, "x", 1);
    CHECK_EQ("NULL key", MR_ERR_NULL, mr_job_run(job));
    mr_job_set_map(job, null_value_map, NULL);
    CHECK_EQ("NULL value with a length", MR_ERR_NULL, mr_job_run(job));
    mr_job_set_map(job, late_error_map, NULL);
    CHECK_EQ("emits after an error do nothing", MR_ERR_NULL, mr_job_run(job));
    mr_job_destroy(job);
    mr_emit(NULL, "k", 1, "v", 1);
    mr_emit_fail(NULL);
    CHECK("mr_emit(NULL, ...) and mr_emit_fail(NULL) do not crash", 1);

    TEST_SECTION("a reduce function that fails");
    job = run_wordcount("a b c", 5, 1, 1, 100, MR_SPLIT_LINES, NULL);
    mr_job_set_reduce(job, fail_reduce, NULL);
    mr_job_set_reducers(job, 3);
    CHECK_EQ("the job returns MR_ERR_CALLBACK", MR_ERR_CALLBACK, mr_job_run(job));
    CHECK_EQ("the output is empty", 0, mr_job_output(job)->count);
    mr_job_destroy(job);

    TEST_SECTION("file problems");
    job = mr_job_create();
    CHECK_EQ("file does not exist", MR_ERR_IO, mr_job_set_input_file(job, "/tmp/mr_no_such_file_xyz"));
    CHECK_EQ("a directory is not a file", MR_ERR_IO, mr_job_set_input_file(job, "/tmp"));
    mr_job_set_map(job, wc_map, NULL);
    mr_job_set_reduce(job, wc_reduce, NULL);
    CHECK_EQ("after the failed reads there is still no input", MR_ERR_STATE, mr_job_run(job));
    mr_job_set_input(job, "a", 1);
    mr_job_run(job);
    CHECK_EQ("writing to a path that cannot be created", MR_ERR_IO,
             mr_job_write_output(job, "/tmp/mr_no_such_dir_xyz/out.txt"));
    CHECK_EQ("NULL output path", MR_ERR_NULL, mr_job_write_output(job, NULL));
    mr_job_destroy(job);

    return TEST_FINISH();
}
