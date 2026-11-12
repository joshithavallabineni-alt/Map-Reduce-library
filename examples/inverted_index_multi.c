/*
 * inverted_index_multi.c
 *
 * Builds an inverted index from multiple text files using the MapReduce
 * framework.
 *
 * Each line is one document. The first token is its document ID.
 * Example:
 *   doc001 the quick brown fox
 *
 * Usage:
 *   ./examples/inverted_index_multi file1.txt file2.txt ... [-- query words...]
 *
 * If "--" is supplied, words after it are searched in the final index.
 */

#include "app_util.h"

/* Map: emit (word, document_id) for every word in every document. */
static void ii_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;

    const char *line;
    size_t line_len, pos = 0;

    while (next_line(input, len, &pos, &line, &line_len)) {
        size_t p = 0;
        const char *doc;
        size_t doc_len;

        /* The first token on a line is the document ID. */
        if (!next_token(line, line_len, &p, &doc, &doc_len))
            continue;

        char word[128];
        size_t word_len;

        /* All remaining tokens are words in this document. */
        while (next_word(line, line_len, &p, word, sizeof(word), &word_len))
            mr_emit(out, word, word_len, doc, doc_len);
    }
}

/* Reduce: remove repeated document IDs and emit one posting list per word. */
static void ii_reduce(const char *key, size_t key_len,
                      const mr_kv_t *values, size_t num_values,
                      mr_emitter_t *out, void *ctx)
{
    (void)ctx;

    size_t total = 0;
    for (size_t i = 0; i < num_values; i++)
        total += values[i].value_len + 1;

    char *list = malloc(total + 1);
    if (list == NULL) {
        mr_emit_fail(out);
        return;
    }

    size_t n = 0;

    for (size_t i = 0; i < num_values; i++) {
        /* Values are sorted, so duplicate document IDs are adjacent. */
        if (i > 0 &&
            values[i].value_len == values[i - 1].value_len &&
            memcmp(values[i].value, values[i - 1].value,
                   values[i].value_len) == 0)
            continue;

        if (n > 0)
            list[n++] = ',';

        memcpy(list + n, values[i].value, values[i].value_len);
        n += values[i].value_len;
    }

    mr_emit(out, key, key_len, list, n);
    free(list);
}

/* Load all input files into one buffer, keeping each document on its own line. */
static char *load_files(int argc, char **argv, int first_file, int end_file,
                        size_t *out_len)
{
    (void)argc;
    char *combined = malloc(1);
    if (combined == NULL)
        return NULL;

    size_t used = 0;
    combined[0] = '\0';

    for (int i = first_file; i < end_file; i++) {
        size_t file_len = 0;
        char *part = read_whole_file(argv[i], &file_len);

        if (part == NULL) {
            fprintf(stderr, "Could not read input file: %s\n", argv[i]);
            free(combined);
            return NULL;
        }

        size_t extra = file_len;
        int add_newline = (file_len > 0 && part[file_len - 1] != '\n');
        if (add_newline)
            extra++;

        char *bigger = realloc(combined, used + extra + 1);
        if (bigger == NULL) {
            free(part);
            free(combined);
            return NULL;
        }
        combined = bigger;

        memcpy(combined + used, part, file_len);
        used += file_len;

        if (add_newline)
            combined[used++] = '\n';

        combined[used] = '\0';
        free(part);
    }

    *out_len = used;
    return combined;
}

/* Independent check: count distinct (word, document) pairs. */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static long reference_pairs(const char *text, size_t len)
{
    size_t cap = 1024, n = 0;
    char **pairs = malloc(cap * sizeof(char *));
    if (pairs == NULL)
        return -1;

    const char *line;
    size_t line_len, pos = 0;

    while (next_line(text, len, &pos, &line, &line_len)) {
        size_t p = 0, doc_len, word_len;
        const char *doc;

        if (!next_token(line, line_len, &p, &doc, &doc_len))
            continue;

        char word[128];

        while (next_word(line, line_len, &p, word, sizeof(word), &word_len)) {
            if (n == cap) {
                cap *= 2;
                char **bigger = realloc(pairs, cap * sizeof(char *));
                if (bigger == NULL) {
                    for (size_t j = 0; j < n; j++)
                        free(pairs[j]);
                    free(pairs);
                    return -1;
                }
                pairs = bigger;
            }

            size_t need = word_len + 1 + doc_len + 1;
            pairs[n] = malloc(need);
            if (pairs[n] == NULL) {
                for (size_t j = 0; j < n; j++)
                    free(pairs[j]);
                free(pairs);
                return -1;
            }

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
        if (i == 0 || strcmp(pairs[i], pairs[i - 1]) != 0)
            distinct++;
    }

    for (size_t i = 0; i < n; i++)
        free(pairs[i]);
    free(pairs);

    return distinct;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s file1.txt [file2.txt ...] [-- query words...]\n",
                argv[0]);
        return 2;
    }

    /* Files appear before "--"; optional search words appear after it. */
    int end_file = argc;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            end_file = i;
            break;
        }
    }

    if (end_file == 1) {
        fprintf(stderr, "Please provide at least one input file.\n");
        return 2;
    }

    size_t input_len = 0;
    char *input = load_files(argc, argv, 1, end_file, &input_len);
    if (input == NULL) {
        fprintf(stderr, "Could not load input files.\n");
        return 1;
    }

    /* 0 mappers means the framework chooses one mapper per split. */
    size_t mappers = 0;
    size_t reducers = 2;

    mr_job_t *job = mr_job_create();
    if (job == NULL) {
        fprintf(stderr, "Could not create MapReduce job.\n");
        free(input);
        return 1;
    }

    mr_error_t err = mr_job_set_map(job, ii_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job, ii_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_LINES);
    if (err == MR_OK) err = mr_job_set_input(job, input, input_len);
    if (err == MR_OK) err = mr_job_run(job);

    if (err != MR_OK) {
        fprintf(stderr, "inverted_index_multi failed: %s\n",
                mr_error_string(err));
        mr_job_destroy(job);
        free(input);
        return 1;
    }

    const mr_kv_list_t *out = mr_job_output(job);

    printf("INVERTED INDEX FROM %d FILE(S)\n", end_file - 1);
    printf("Mappers used: %zu | Reducers: %zu\n\n",
           mr_job_stats(job)->mappers_used, reducers);

    for (size_t i = 0; i < out->count; i++)
        printf("%-15s -> %s\n", out->items[i].key, out->items[i].value);

    /* Optional queries: everything after "--". */
    for (int q = end_file + 1; q < argc; q++) {
        int found = 0;
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->items[i].key, argv[q]) == 0) {
                printf("\nQuery \"%s\": %s\n",
                       argv[q], out->items[i].value);
                found = 1;
                break;
            }
        }
        if (!found)
            printf("\nQuery \"%s\": no document found\n", argv[q]);
    }

    /* Verify the number of listed word-document pairs independently. */
    long listed = 0;
    for (size_t i = 0; i < out->count; i++) {
        listed++;
        for (size_t k = 0; k < out->items[i].value_len; k++) {
            if (out->items[i].value[k] == ',')
                listed++;
        }
    }

    long expected = reference_pairs(input, input_len);
    printf("\nCheck: %ld entries in index, %ld expected -> %s\n",
           listed, expected,
           (expected >= 0 && listed == expected) ? "OK" : "MISMATCH");

    print_stats(job);

    int exit_code = (expected >= 0 && listed == expected) ? 0 : 1;
    mr_job_destroy(job);
    free(input);
    return exit_code;
}
