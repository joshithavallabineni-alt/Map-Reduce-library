/*
 * max_per_key.c - application 3: MAXIMUM VALUE PER KEY
 *
 * Input: lines "key number", for example daily temperatures "delhi 31.5".
 *   map    : parse the line, emit (key, number as text)
 *   reduce : look at all numbers of one key, emit (key, the largest)
 *
 * Usage: ./examples/max_per_key [file|-] [max-mappers] [reducers]
 */
#include <float.h>

#include "app_util.h"

static const char SAMPLE[] =
    "delhi 31.5\nchennai 33.0\ndelhi 35.2\nmumbai 29.8\nchennai 34.1\n"
    "mumbai 30.4\ndelhi 29.0\nkolkata 32.2\nmumbai 31.9\nkolkata 28.5\n"
    "chennai 31.7\nkolkata 33.6\n";

static void max_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    const char *line;
    size_t line_len, pos = 0;
    while (next_line(input, len, &pos, &line, &line_len)) {
        size_t p = 0, key_len, num_len;
        const char *key, *num;
        if (!next_token(line, line_len, &p, &key, &key_len)) continue;   /* blank line */
        if (!next_token(line, line_len, &p, &num, &num_len)) continue;   /* no number  */
        int ok;
        token_to_double(num, num_len, &ok);
        if (!ok) continue;                                               /* not a number */
        mr_emit(out, key, key_len, num, num_len);
    }
}

static void max_reduce(const char *key, size_t key_len,
                       const mr_kv_t *values, size_t num_values,
                       mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    double best = -DBL_MAX;
    for (size_t i = 0; i < num_values; i++) {
        double v = strtod(values[i].value, NULL);     /* library adds a '\0' */
        if (v > best) best = v;
    }
    char text[64];
    int n = snprintf(text, sizeof(text), "%.10g", best);
    mr_emit(out, key, key_len, text, (size_t)n);
}

/* Independent check: a table of (key, max) filled by one plain loop. */
typedef struct { char key[64]; double max; } row_t;

static int reference_check(const char *text, size_t len, const mr_kv_list_t *out)
{
    size_t cap = 64, rows = 0;
    row_t *table = malloc(cap * sizeof(row_t));
    if (table == NULL) return 0;
    const char *line;
    size_t line_len, pos = 0;
    while (next_line(text, len, &pos, &line, &line_len)) {
        size_t p = 0, key_len, num_len;
        const char *key, *num;
        if (!next_token(line, line_len, &p, &key, &key_len)) continue;
        if (!next_token(line, line_len, &p, &num, &num_len)) continue;
        int ok;
        double v = token_to_double(num, num_len, &ok);
        if (!ok || key_len >= sizeof(table[0].key)) continue;
        size_t r;
        for (r = 0; r < rows; r++)
            if (strlen(table[r].key) == key_len && memcmp(table[r].key, key, key_len) == 0) break;
        if (r == rows) {
            if (rows == cap) {
                cap *= 2;
                row_t *bigger = realloc(table, cap * sizeof(row_t));
                if (bigger == NULL) { free(table); return 0; }
                table = bigger;
            }
            memcpy(table[r].key, key, key_len);
            table[r].key[key_len] = '\0';
            table[r].max = v;
            rows++;
        } else if (v > table[r].max) {
            table[r].max = v;
        }
    }
    int good = (rows == out->count);
    for (size_t i = 0; good && i < out->count; i++) {
        size_t r;
        for (r = 0; r < rows; r++)
            if (strcmp(table[r].key, out->items[i].key) == 0) break;
        if (r == rows) good = 0;
        else {
            double got = strtod(out->items[i].value, NULL);
            double diff = got - table[r].max;
            if (diff < 0) diff = -diff;
            if (diff > 1e-6 * (1.0 + (table[r].max < 0 ? -table[r].max : table[r].max))) good = 0;
        }
    }
    free(table);
    return good;
}

int main(int argc, char **argv)
{
    const char *file = (argc > 1) ? argv[1] : "-";
    size_t mappers = 0, reducers = 2;     /* mappers 0 = one per split */
    if (argc > 2 && !parse_positive(argv[2], &mappers)) { fprintf(stderr, "bad mappers\n"); return 2; }
    if (argc > 3 && !parse_positive(argv[3], &reducers)) { fprintf(stderr, "bad reducers\n"); return 2; }

    mr_job_t *job = mr_job_create();
    if (job == NULL) { fprintf(stderr, "out of memory\n"); return 1; }

    mr_error_t err = mr_job_set_map(job, max_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job, max_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_LINES);
    if (err == MR_OK) {
        if (strcmp(file, "-") == 0) {
            err = mr_job_set_split_size(job, 30);
            if (err == MR_OK) err = mr_job_set_input(job, SAMPLE, sizeof(SAMPLE) - 1);
        } else {
            err = mr_job_set_input_file(job, file);
        }
    }
    if (err == MR_OK) err = mr_job_run(job);
    if (err != MR_OK) {
        fprintf(stderr, "max_per_key failed: %s\n", mr_error_string(err));
        mr_job_destroy(job);
        return 1;
    }

    const mr_kv_list_t *out = mr_job_output(job);
    printf("MAXIMUM VALUE PER KEY  (%zu mappers = one per split, %zu reducers)\n\n", mr_job_stats(job)->mappers_used, reducers);
    size_t show = out->count < 40 ? out->count : 40;
    for (size_t i = 0; i < show; i++)
        printf("  %-12s %s\n", out->items[i].key, out->items[i].value);
    if (show < out->count) printf("  ... (%zu more keys)\n", out->count - show);

    int exit_code = 1;
    const char *text = strcmp(file, "-") == 0 ? SAMPLE : NULL;
    size_t text_len = sizeof(SAMPLE) - 1;
    char *loaded = NULL;
    if (text == NULL) {
        loaded = read_whole_file(file, &text_len);
        text = loaded;
    }
    if (text != NULL) {
        int good = reference_check(text, text_len, out);
        printf("\nCheck against a plain loop: %s\n", good ? "OK" : "MISMATCH");
        exit_code = good ? 0 : 1;
    }
    print_stats(job);
    free(loaded);
    mr_job_destroy(job);
    return exit_code;
}
