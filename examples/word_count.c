/*
 * word_count.c - application 1: WORD COUNT
 *
 * How many times does each word occur?
 *   map    : for every word in the split, emit (word, "1")
 *   reduce : add up the "1"s of one word, emit (word, total)
 *
 * Usage: ./examples/word_count [file|-] [max-mappers] [reducers]
 *   "-" (or no file) uses a small built-in text.
 * The program checks the result against a simple one-pass count and exits
 * with 1 if they differ.
 */
#include "app_util.h"

static const char SAMPLE[] =
    "the cat sat on the mat\n"
    "the dog sat on the log\n"
    "the cat and the dog are friends\n"
    "The End\n";

/* ---- the user's map function ---------------------------------------- */
static void wc_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    char word[128];
    size_t word_len, pos = 0;
    while (next_word(input, len, &pos, word, sizeof(word), &word_len)) {
        mr_emit(out, word, word_len, "1", 1);
    }
}

/* ---- the user's reduce function --------------------------------------- */
static void wc_reduce(const char *key, size_t key_len,
                      const mr_kv_t *values, size_t num_values,
                      mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    unsigned long total = 0;
    for (size_t i = 0; i < num_values; i++) total += strtoul(values[i].value, NULL, 10);
    char text[32];
    int n = snprintf(text, sizeof(text), "%lu", total);
    mr_emit(out, key, key_len, text, (size_t)n);
}

/* Independent check: count all words in a single pass. */
static unsigned long count_words(const char *text, size_t len)
{
    char word[128];
    size_t word_len, pos = 0;
    unsigned long n = 0;
    while (next_word(text, len, &pos, word, sizeof(word), &word_len)) n++;
    return n;
}

int main(int argc, char **argv)
{
    const char *file = (argc > 1) ? argv[1] : "-";
    size_t mappers = 0, reducers = 2;     /* mappers 0 = one per split */
    if (argc > 2 && !parse_positive(argv[2], &mappers)) { fprintf(stderr, "bad mappers\n"); return 2; }
    if (argc > 3 && !parse_positive(argv[3], &reducers)) { fprintf(stderr, "bad reducers\n"); return 2; }

    mr_job_t *job = mr_job_create();
    if (job == NULL) { fprintf(stderr, "out of memory\n"); return 1; }

    int exit_code = 1;
    mr_error_t err = MR_OK;
    if (err == MR_OK) err = mr_job_set_map(job, wc_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job, wc_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_WORDS);
    if (err == MR_OK) {
        if (strcmp(file, "-") == 0) {
            err = mr_job_set_split_size(job, 16);        /* tiny splits: several mappers get work */
            if (err == MR_OK) err = mr_job_set_input(job, SAMPLE, sizeof(SAMPLE) - 1);
        } else {
            err = mr_job_set_input_file(job, file);
        }
    }
    if (err == MR_OK) err = mr_job_run(job);
    if (err != MR_OK) {
        fprintf(stderr, "word_count failed: %s\n", mr_error_string(err));
        mr_job_destroy(job);
        return 1;
    }

    const mr_kv_list_t *out = mr_job_output(job);
    printf("WORD COUNT  (%zu mappers = one per split, %zu reducers)\n\n", mr_job_stats(job)->mappers_used, reducers);
    size_t show = out->count < 40 ? out->count : 40;
    for (size_t i = 0; i < show; i++)
        printf("  %-14s %s\n", out->items[i].key, out->items[i].value);
    if (show < out->count) printf("  ... (%zu more words)\n", out->count - show);

    /* Verify: the counts must add up to the number of words in the input. */
    const char *text = strcmp(file, "-") == 0 ? SAMPLE : NULL;
    size_t text_len = sizeof(SAMPLE) - 1;
    char *loaded = NULL;
    if (text == NULL) {                      /* read the file again, independently */
        loaded = read_whole_file(file, &text_len);
        text = loaded;
    }
    if (text != NULL) {
        unsigned long expected = count_words(text, text_len), total = 0;
        for (size_t i = 0; i < out->count; i++) total += strtoul(out->items[i].value, NULL, 10);
        printf("\nCheck: %lu words counted by the job, %lu by a plain loop -> %s\n",
               total, expected, total == expected ? "OK" : "MISMATCH");
        exit_code = (total == expected) ? 0 : 1;
    }
    print_stats(job);
    free(loaded);
    mr_job_destroy(job);
    return exit_code;
}
