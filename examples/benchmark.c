/*
 * benchmark.c - WORKER ANALYSIS: how many mappers and reducers are best?
 *
 * Runs a word-count job on a large generated text with different numbers of
 * mapper and reducer threads, repeats every setting several times, and prints
 *   Table 1  equal numbers of mappers and reducers: time, speed-up, efficiency
 *   Table 2  every combination of mappers x reducers (mean time)
 *   Table 3  the effect of the split size
 * "mean +- sd" is the mean and the standard deviation over the repeated runs.
 *
 * Usage: ./examples/benchmark [words] [runs]      (default 2000000 3)
 * Every run is checked: the counts must add up to the number of words.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <unistd.h>

#include "app_util.h"

/* ---- the job: plain word count ------------------------------------ */
static void bench_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    char word[128];
    size_t word_len, pos = 0;
    while (next_word(input, len, &pos, word, sizeof(word), &word_len))
        mr_emit(out, word, word_len, "1", 1);
}

static void bench_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                         size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    unsigned long total = 0;
    for (size_t i = 0; i < num_values; i++) total += strtoul(values[i].value, NULL, 10);
    char text[32];
    int n = snprintf(text, sizeof(text), "%lu", total);
    mr_emit(out, key, key_len, text, (size_t)n);
}

/* ---- test data: skewed vocabulary, 10 words per line ---------------- */
static unsigned long long rng_state = 88172645463325252ULL;
static double rng01(void)                       /* xorshift64, repeatable */
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) / 9007199254740992.0;
}

static char *make_text(size_t words, size_t *out_len)
{
    const unsigned vocabulary = 5000;
    char *text = malloc(words * 12 + 16);
    if (text == NULL) return NULL;
    size_t len = 0;
    for (size_t i = 0; i < words; i++) {
        double u = rng01();
        unsigned id = (unsigned)(u * u * (double)vocabulary);   /* low ids are far more common */
        len += (size_t)sprintf(text + len, "word%u%c", id, (i % 10 == 9) ? '\n' : ' ');
    }
    text[len++] = '\n';
    *out_len = len;
    return text;
}

/* ---- measuring ------------------------------------------------------ */
typedef struct {
    double mean, sd;                  /* total time */
    double map, shuffle, reduce;      /* mean phase times */
    int ok;
} result_t;

static result_t measure(const char *text, size_t len, size_t words, size_t mappers,
                        size_t reducers, size_t split_size, int runs)
{
    result_t r;
    memset(&r, 0, sizeof(r));
    r.ok = 1;
    double sum = 0, sumsq = 0;
    for (int run = 0; run < runs; run++) {
        mr_job_t *job = mr_job_create();
        mr_error_t err = (job == NULL) ? MR_ERR_NO_MEMORY : MR_OK;
        if (err == MR_OK) err = mr_job_set_map(job, bench_map, NULL);
        if (err == MR_OK) err = mr_job_set_reduce(job, bench_reduce, NULL);
        if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
        if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
        if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_LINES);
        if (err == MR_OK) err = mr_job_set_split_size(job, split_size);
        if (err == MR_OK) err = mr_job_set_input(job, text, len);
        if (err == MR_OK) err = mr_job_run(job);
        if (err != MR_OK) {
            r.ok = 0;
            mr_job_destroy(job);
            return r;
        }
        const mr_kv_list_t *out = mr_job_output(job);
        unsigned long total = 0;
        for (size_t i = 0; i < out->count; i++) total += strtoul(out->items[i].value, NULL, 10);
        if (total != words) r.ok = 0;                 /* wrong answer! */

        const mr_job_stats_t *s = mr_job_stats(job);
        sum += s->total_ms;
        sumsq += s->total_ms * s->total_ms;
        r.map += s->map_ms;
        r.shuffle += s->shuffle_ms;
        r.reduce += s->reduce_ms;
        mr_job_destroy(job);
    }
    r.mean = sum / runs;
    double var = runs > 1 ? (sumsq - runs * r.mean * r.mean) / (runs - 1) : 0.0;
    r.sd = var > 0 ? sqrt(var) : 0.0;
    r.map /= runs;
    r.shuffle /= runs;
    r.reduce /= runs;
    return r;
}

int main(int argc, char **argv)
{
    size_t words = 2000000, runs_arg = 3;
    if (argc > 1 && !parse_positive(argv[1], &words)) { fprintf(stderr, "bad word count\n"); return 2; }
    if (argc > 2 && !parse_positive(argv[2], &runs_arg)) { fprintf(stderr, "bad runs\n"); return 2; }
    int runs = (int)runs_arg;

    size_t len;
    char *text = make_text(words, &len);
    if (text == NULL) { fprintf(stderr, "out of memory\n"); return 1; }

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("WORKER ANALYSIS: word count of %zu words (%.1f MB), %d run(s) per setting\n",
           words, (double)len / 1048576.0, runs);
    printf("Processor cores available: %ld  (speed-up cannot exceed this)\n", cores);

    int all_ok = 1;
    size_t counts[] = { 1, 2, 3, 4, 6, 8 };
    size_t num_counts = sizeof(counts) / sizeof(counts[0]);
    size_t split = 262144;                           /* 256 KB: plenty of splits for 8 mappers */

    /* ---- Table 1 ---- */
    printf("\nTable 1: mappers = reducers = w   (split size %zu KB)\n", split / 1024);
    printf("  %3s  %17s  %8s  %10s   %8s %8s %8s\n", "w", "total ms (mean+-sd)", "speed-up",
           "efficiency", "map", "shuffle", "reduce");
    double base = 0, best_time = 1e30;
    size_t best_w = 1;
    for (size_t i = 0; i < num_counts; i++) {
        result_t r = measure(text, len, words, counts[i], counts[i], split, runs);
        all_ok &= r.ok;
        if (i == 0) base = r.mean;
        double speedup = base / r.mean;
        printf("  %3zu  %9.1f +- %5.1f  %8.2f  %9.0f%%   %8.1f %8.1f %8.1f%s\n", counts[i], r.mean,
               r.sd, speedup, 100.0 * speedup / (double)counts[i], r.map, r.shuffle, r.reduce,
               r.ok ? "" : "   WRONG RESULT");
        if (r.mean < best_time) { best_time = r.mean; best_w = counts[i]; }
    }
    printf("  fastest setting: w = %zu (%.1f ms)\n", best_w, best_time);

    /* ---- Table 2 ---- */
    size_t grid[] = { 1, 2, 4, 8 };
    printf("\nTable 2: total time in ms (mean), mappers (rows) x reducers (columns)\n");
    printf("  %8s", "");
    for (int c = 0; c < 4; c++) printf("  red=%-5zu", grid[c]);
    printf("\n");
    double grid_best = 1e30;
    size_t best_m = 1, best_r = 1;
    for (int a = 0; a < 4; a++) {
        printf("  map=%-4zu", grid[a]);
        for (int c = 0; c < 4; c++) {
            result_t r = measure(text, len, words, grid[a], grid[c], split, runs);
            all_ok &= r.ok;
            printf("  %9.1f", r.mean);
            if (r.mean < grid_best) { grid_best = r.mean; best_m = grid[a]; best_r = grid[c]; }
        }
        printf("\n");
    }
    printf("  fastest combination: %zu mappers, %zu reducers (%.1f ms)\n", best_m, best_r, grid_best);

    /* ---- Table 3 ---- */
    size_t w = (cores >= 2 && cores <= 8) ? (size_t)cores : 4;
    size_t sizes[] = { 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216 };
    printf("\nTable 3: split size with %zu mappers and %zu reducers\n", w, w);
    printf("  %10s  %8s  %17s\n", "split size", "splits", "total ms (mean+-sd)");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        result_t r = measure(text, len, words, w, w, sizes[i], runs);
        all_ok &= r.ok;
        size_t splits = len / sizes[i] + 1;
        printf("  %7zu KB  %8zu  %9.1f +- %5.1f\n", sizes[i] / 1024, splits, r.mean, r.sd);
    }

    printf("\nAll results correct: %s\n", all_ok ? "yes" : "NO");
    free(text);
    return all_ok ? 0 : 1;
}
