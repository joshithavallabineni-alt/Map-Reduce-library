/*
 * word_frequency.c - application 4: WORD FREQUENCY ANALYSIS (two chained jobs)
 *
 * Job 1 (word count):      text  ->  (word, how many times)
 * Job 2 (frequency of frequencies):
 *     input = the output of job 1, one line per word:  "word<TAB>count"
 *     map    : emit (count, "1")
 *     reduce : add up -> (count, how many different words occur that often)
 * Then the program prints the most frequent words with their share of all
 * words, and the histogram from job 2. It shows that the output of one job can
 * be the input of the next.
 *
 * Usage: ./examples/word_frequency [file|-] [max-mappers] [reducers]
 */
#include "app_util.h"

static const char SAMPLE[] =
    "to be or not to be that is the question\n"
    "whether tis nobler in the mind to suffer\n"
    "the slings and arrows of outrageous fortune\n"
    "or to take arms against a sea of troubles\n"
    "and by opposing end them to die to sleep no more\n";

/* ---- job 1: word count -------------------------------------------- */
static void count_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    char word[128];
    size_t word_len, pos = 0;
    while (next_word(input, len, &pos, word, sizeof(word), &word_len))
        mr_emit(out, word, word_len, "1", 1);
}

static void count_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                         size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    unsigned long total = 0;
    for (size_t i = 0; i < num_values; i++) total += strtoul(values[i].value, NULL, 10);
    char text[32];
    int n = snprintf(text, sizeof(text), "%lu", total);
    mr_emit(out, key, key_len, text, (size_t)n);
}

/* ---- job 2: how many words have each count ------------------------- */
static void hist_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    const char *line;
    size_t line_len, pos = 0;
    while (next_line(input, len, &pos, &line, &line_len)) {
        size_t t = 0;
        while (t < line_len && line[t] != '\t') t++;           /* skip the word */
        if (t >= line_len) continue;
        unsigned long count = 0;
        for (size_t i = t + 1; i < line_len && isdigit((unsigned char)line[i]); i++)
            count = count * 10 + (unsigned long)(line[i] - '0');
        char key[32];
        int n = snprintf(key, sizeof(key), "%010lu", count);   /* zero-padded: sorts as a number */
        mr_emit(out, key, (size_t)n, "1", 1);
    }
}

/* ---- sorting the job-1 output for printing ------------------------- */
typedef struct { const char *word; unsigned long count; } entry_t;

static int by_count_desc(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;
    return strcmp(x->word, y->word);
}

int main(int argc, char **argv)
{
    const char *file = (argc > 1) ? argv[1] : "-";
    size_t mappers = 0, reducers = 2;     /* mappers 0 = one per split */
    if (argc > 2 && !parse_positive(argv[2], &mappers)) { fprintf(stderr, "bad mappers\n"); return 2; }
    if (argc > 3 && !parse_positive(argv[3], &reducers)) { fprintf(stderr, "bad reducers\n"); return 2; }

    mr_job_t *job1 = mr_job_create();
    mr_job_t *job2 = mr_job_create();
    char *lines = NULL;
    entry_t *entries = NULL;
    int exit_code = 1;
    if (job1 == NULL || job2 == NULL) { fprintf(stderr, "out of memory\n"); goto done; }

    /* ---- job 1 ---- */
    mr_error_t err = mr_job_set_map(job1, count_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job1, count_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job1, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job1, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job1, MR_SPLIT_WORDS);
    if (err == MR_OK) {
        if (strcmp(file, "-") == 0) {
            err = mr_job_set_split_size(job1, 40);
            if (err == MR_OK) err = mr_job_set_input(job1, SAMPLE, sizeof(SAMPLE) - 1);
        } else {
            err = mr_job_set_input_file(job1, file);
        }
    }
    if (err == MR_OK) err = mr_job_run(job1);
    if (err != MR_OK) { fprintf(stderr, "job 1 failed: %s\n", mr_error_string(err)); goto done; }

    const mr_kv_list_t *counts = mr_job_output(job1);
    unsigned long total_words = 0;
    for (size_t i = 0; i < counts->count; i++) total_words += strtoul(counts->items[i].value, NULL, 10);

    /* ---- build the input of job 2 from the output of job 1 ---- */
    size_t need = 16;
    for (size_t i = 0; i < counts->count; i++)
        need += counts->items[i].key_len + counts->items[i].value_len + 2;
    lines = malloc(need);
    entries = malloc((counts->count + 1) * sizeof(entry_t));
    if (lines == NULL || entries == NULL) { fprintf(stderr, "out of memory\n"); goto done; }
    size_t len = 0;
    for (size_t i = 0; i < counts->count; i++) {
        len += (size_t)snprintf(lines + len, need - len, "%s\t%s\n",
                                counts->items[i].key, counts->items[i].value);
        entries[i].word = counts->items[i].key;
        entries[i].count = strtoul(counts->items[i].value, NULL, 10);
    }

    /* ---- job 2 ---- */
    err = mr_job_set_map(job2, hist_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job2, count_reduce, NULL);   /* same "add up" reduce */
    if (err == MR_OK) err = mr_job_set_mappers(job2, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job2, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job2, MR_SPLIT_LINES);
    if (err == MR_OK) err = mr_job_set_input(job2, lines, len);
    if (err == MR_OK) err = mr_job_run(job2);
    if (err != MR_OK) { fprintf(stderr, "job 2 failed: %s\n", mr_error_string(err)); goto done; }

    printf("WORD FREQUENCY ANALYSIS  (job 1: %zu mappers = one per split, %zu reducers)\n", mr_job_stats(job1)->mappers_used, reducers);
    printf("\n%lu words in total, %zu different words\n", total_words, counts->count);

    qsort(entries, counts->count, sizeof(entry_t), by_count_desc);
    printf("\nMost frequent words:\n");
    printf("  %-14s %7s %9s\n", "word", "count", "share");
    for (size_t i = 0; i < counts->count && i < 10; i++)
        printf("  %-14s %7lu %8.2f%%\n", entries[i].word, entries[i].count,
               100.0 * (double)entries[i].count / (double)total_words);

    const mr_kv_list_t *hist = mr_job_output(job2);
    printf("\nFrequency of frequencies (job 2):\n");
    unsigned long words_in_hist = 0, words_weighted = 0;
    for (size_t i = 0; i < hist->count; i++) {
        unsigned long c = strtoul(hist->items[i].key, NULL, 10);
        unsigned long w = strtoul(hist->items[i].value, NULL, 10);
        words_in_hist += w;
        words_weighted += c * w;
        if (i < 15) printf("  %lu word(s) occur %lu time(s)\n", w, c);
    }
    if (hist->count > 15) printf("  ... (%zu more rows)\n", hist->count - 15);

    int ok = (words_in_hist == counts->count) && (words_weighted == total_words);
    printf("\nCheck: histogram covers %lu different words and %lu words in total -> %s\n",
           words_in_hist, words_weighted, ok ? "OK" : "MISMATCH");
    print_stats(job1);
    exit_code = ok ? 0 : 1;

done:
    free(lines);
    free(entries);
    mr_job_destroy(job1);
    mr_job_destroy(job2);
    return exit_code;
}
