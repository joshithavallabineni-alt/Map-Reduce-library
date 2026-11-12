/*
 * inverted_index.c - application 2: INVERTED INDEX
 *
 * For every word, which documents contain it?
 * Input: one document per line, the first token of the line is the document id:
 *        doc1 the quick brown fox
 *   map    : for every word of the line, emit (word, document id)
 *   reduce : sort-and-deduplicate the ids of one word, emit (word, "doc1,doc3")
 *
 * Usage: ./examples/inverted_index [file|-] [max-mappers] [reducers] [query words...]
 */
#include "app_util.h"

static const char SAMPLE[] =
    "doc1 the cat sat on the mat\n"
    "doc2 the dog sat on the log\n"
    "doc3 cats and dogs are friends\n"
    "doc4 the cat chased the dog\n"
    "doc5 a log and a mat\n";

/* ---- map: one split = whole lines (split mode MR_SPLIT_LINES) ---------- */
static void ii_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    const char *line;
    size_t line_len, pos = 0;
    while (next_line(input, len, &pos, &line, &line_len)) {
        size_t p = 0;
        const char *doc;
        size_t doc_len;
        if (!next_token(line, line_len, &p, &doc, &doc_len)) continue;   /* empty line */
        char word[128];
        size_t word_len;
        while (next_word(line, line_len, &p, word, sizeof(word), &word_len)) {
            mr_emit(out, word, word_len, doc, doc_len);
        }
    }
}

/* ---- reduce: values arrive sorted, so duplicates are neighbours ------- */
static void ii_reduce(const char *key, size_t key_len,
                      const mr_kv_t *values, size_t num_values,
                      mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    size_t total = 0;
    for (size_t i = 0; i < num_values; i++) total += values[i].value_len + 1;
    char *list = malloc(total + 1);
    if (list == NULL) { mr_emit_fail(out); return; }

    size_t n = 0;
    for (size_t i = 0; i < num_values; i++) {
        if (i > 0 && values[i].value_len == values[i - 1].value_len &&
            memcmp(values[i].value, values[i - 1].value, values[i].value_len) == 0)
            continue;                                   /* same document again */
        if (n > 0) list[n++] = ',';
        memcpy(list + n, values[i].value, values[i].value_len);
        n += values[i].value_len;
    }
    mr_emit(out, key, key_len, list, n);
    free(list);
}

/* ---- independent check: count distinct (word, doc) pairs by sorting ---- */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static long reference_pairs(const char *text, size_t len)
{
    size_t cap = 1024, n = 0;
    char **pairs = malloc(cap * sizeof(char *));
    if (pairs == NULL) return -1;
    const char *line;
    size_t line_len, pos = 0;
    while (next_line(text, len, &pos, &line, &line_len)) {
        size_t p = 0, doc_len, word_len;
        const char *doc;
        if (!next_token(line, line_len, &p, &doc, &doc_len)) continue;
        char word[128];
        while (next_word(line, line_len, &p, word, sizeof(word), &word_len)) {
            if (n == cap) {
                cap *= 2;
                char **bigger = realloc(pairs, cap * sizeof(char *));
                if (bigger == NULL) return -1;
                pairs = bigger;
            }
            size_t need = word_len + 1 + doc_len + 1;
            pairs[n] = malloc(need);
            if (pairs[n] == NULL) return -1;
            memcpy(pairs[n], word, word_len);
            pairs[n][word_len] = ' ';
            memcpy(pairs[n] + word_len + 1, doc, doc_len);
            pairs[n][need - 1] = '\0';
            n++;
        }
    }
    qsort(pairs, n, sizeof(char *), cmp_str);
    long distinct = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == 0 || strcmp(pairs[i], pairs[i - 1]) != 0) distinct++;
    }
    for (size_t i = 0; i < n; i++) free(pairs[i]);   /* free only after comparing */
    free(pairs);
    return distinct;
}

int main(int argc, char **argv)
{
    const char *file = (argc > 1) ? argv[1] : "-";
    size_t mappers = 0, reducers = 2;     /* mappers 0 = one per split */
    if (argc > 2 && !parse_positive(argv[2], &mappers)) { fprintf(stderr, "bad mappers\n"); return 2; }
    if (argc > 3 && !parse_positive(argv[3], &reducers)) { fprintf(stderr, "bad reducers\n"); return 2; }

    mr_job_t *job = mr_job_create();
    if (job == NULL) { fprintf(stderr, "out of memory\n"); return 1; }

    mr_error_t err = mr_job_set_map(job, ii_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job, ii_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_LINES);   /* never cut a document */
    if (err == MR_OK) {
        if (strcmp(file, "-") == 0) {
            err = mr_job_set_split_size(job, 40);
            if (err == MR_OK) err = mr_job_set_input(job, SAMPLE, sizeof(SAMPLE) - 1);
        } else {
            err = mr_job_set_input_file(job, file);
        }
    }
    if (err == MR_OK) err = mr_job_run(job);
    if (err != MR_OK) {
        fprintf(stderr, "inverted_index failed: %s\n", mr_error_string(err));
        mr_job_destroy(job);
        return 1;
    }

    const mr_kv_list_t *out = mr_job_output(job);
    printf("INVERTED INDEX  (%zu mappers = one per split, %zu reducers)\n\n", mr_job_stats(job)->mappers_used, reducers);
    size_t show = out->count < 40 ? out->count : 40;
    for (size_t i = 0; i < show; i++)
        printf("  %-12s -> %s\n", out->items[i].key, out->items[i].value);
    if (show < out->count) printf("  ... (%zu more words)\n", out->count - show);

    for (int q = 4; q < argc; q++) {                 /* optional queries */
        int found = 0;
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->items[i].key, argv[q]) == 0) {
                printf("\nQuery \"%s\": %s\n", argv[q], out->items[i].value);
                found = 1;
            }
        }
        if (!found) printf("\nQuery \"%s\": no document\n", argv[q]);
    }

    /* Verify: total number of ids in all posting lists = distinct (word, doc) pairs. */
    int exit_code = 1;
    const char *text = strcmp(file, "-") == 0 ? SAMPLE : NULL;
    size_t text_len = sizeof(SAMPLE) - 1;
    char *loaded = NULL;
    if (text == NULL) {
        loaded = read_whole_file(file, &text_len);
        text = loaded;
    }
    if (text != NULL) {
        long listed = 0;
        for (size_t i = 0; i < out->count; i++) {
            listed++;
            for (size_t k = 0; k < out->items[i].value_len; k++)
                if (out->items[i].value[k] == ',') listed++;
        }
        long expected = reference_pairs(text, text_len);
        printf("\nCheck: %ld (word, document) entries in the index, %ld by a plain loop -> %s\n",
               listed, expected, listed == expected ? "OK" : "MISMATCH");
        exit_code = (listed == expected) ? 0 : 1;
    }
    print_stats(job);
    free(loaded);
    mr_job_destroy(job);
    return exit_code;
}
