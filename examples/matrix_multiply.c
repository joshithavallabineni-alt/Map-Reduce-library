#include "app_util.h"

typedef struct { size_t n, m, p; } dims_t;

static void mm_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    const dims_t *d = ctx;
    const char *line;
    size_t line_len, pos = 0;
    while (next_line(input, len, &pos, &line, &line_len)) {
        size_t p = 0, t1, t2, t3, t4;
        const char *which, *a, *b, *val;
        if (!next_token(line, line_len, &p, &which, &t1)) continue;
        if (!next_token(line, line_len, &p, &a, &t2)) continue;
        if (!next_token(line, line_len, &p, &b, &t3)) continue;
        if (!next_token(line, line_len, &p, &val, &t4)) continue;
        if (t1 != 1 || t2 > 9 || t3 > 9 || t4 > 20) continue;

        char first[16], second[16], value[24];
        memcpy(first, a, t2);  first[t2] = '\0';
        memcpy(second, b, t3); second[t3] = '\0';
        memcpy(value, val, t4); value[t4] = '\0';
        size_t x = strtoul(first, NULL, 10), y = strtoul(second, NULL, 10);

        char key[40], v[64];
        if (which[0] == 'A') {                      /* A[i=x][j=y] */
            for (size_t k = 0; k < d->p; k++) {
                int kn = snprintf(key, sizeof(key), "%zu,%zu", x, k);
                int vn = snprintf(v, sizeof(v), "A,%zu,%s", y, value);
                mr_emit(out, key, (size_t)kn, v, (size_t)vn);
            }
        } else if (which[0] == 'B') {               /* B[j=x][k=y] */
            for (size_t i = 0; i < d->n; i++) {
                int kn = snprintf(key, sizeof(key), "%zu,%zu", i, y);
                int vn = snprintf(v, sizeof(v), "B,%zu,%s", x, value);
                mr_emit(out, key, (size_t)kn, v, (size_t)vn);
            }
        }
    }
}

static void mm_reduce(const char *key, size_t key_len,
                      const mr_kv_t *values, size_t num_values,
                      mr_emitter_t *out, void *ctx)
{
    const dims_t *d = ctx;
    long *a = calloc(d->m, sizeof(long));
    long *b = calloc(d->m, sizeof(long));
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        mr_emit_fail(out);
        return;
    }
    for (size_t i = 0; i < num_values; i++) {
        const char *v = values[i].value;            /* "A,j,value" or "B,j,value" */
        char *end;
        size_t j = strtoul(v + 2, &end, 10);
        long x = strtol(end + 1, NULL, 10);
        if (j >= d->m) continue;
        if (v[0] == 'A') a[j] = x; else b[j] = x;
    }
    long sum = 0;
    for (size_t j = 0; j < d->m; j++) sum += a[j] * b[j];
    free(a);
    free(b);

    char text[32];
    int n = snprintf(text, sizeof(text), "%ld", sum);
    mr_emit(out, key, key_len, text, (size_t)n);
}

int main(int argc, char **argv)
{
    size_t n = 6, m = 4, p = 5, mappers = 0, reducers = 2;   /* mappers 0 = one per split */
    int a = 1;
    if (argc >= 4) {
        if (!parse_positive(argv[1], &n) || !parse_positive(argv[2], &m) ||
            !parse_positive(argv[3], &p)) {
            fprintf(stderr, "usage: matrix_multiply [n m p] [max-mappers] [reducers]\n");
            return 2;
        }
        a = 4;
    }
    if (argc > a && !parse_positive(argv[a], &mappers)) { fprintf(stderr, "bad mappers\n"); return 2; }
    if (argc > a + 1 && !parse_positive(argv[a + 1], &reducers)) { fprintf(stderr, "bad reducers\n"); return 2; }
    if (n > 100000 || m > 100000 || p > 100000 || n * m > 4000000 || m * p > 4000000) {
        fprintf(stderr, "matrices too large for this example\n");
        return 2;
    }

    /* Fill A and B with small whole numbers (fixed formula: repeatable). */
    long *A = malloc(n * m * sizeof(long)), *B = malloc(m * p * sizeof(long));
    long *C = calloc(n * p, sizeof(long));
    if (A == NULL || B == NULL || C == NULL) { fprintf(stderr, "out of memory\n"); return 1; }
    for (size_t i = 0; i < n; i++) for (size_t j = 0; j < m; j++) A[i * m + j] = (long)((i * 7 + j * 3 + i * j) % 10);
    for (size_t j = 0; j < m; j++) for (size_t k = 0; k < p; k++) B[j * p + k] = (long)((j * 3 + k * 7 + j * k + 1) % 10);

    /* The input text: one line per element. */
    size_t cap = (n * m + m * p) * 24 + 16, len = 0;
    char *text = malloc(cap);
    if (text == NULL) { fprintf(stderr, "out of memory\n"); return 1; }
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < m; j++)
            len += (size_t)snprintf(text + len, cap - len, "A %zu %zu %ld\n", i, j, A[i * m + j]);
    for (size_t j = 0; j < m; j++)
        for (size_t k = 0; k < p; k++)
            len += (size_t)snprintf(text + len, cap - len, "B %zu %zu %ld\n", j, k, B[j * p + k]);

    dims_t dims = { n, m, p };
    mr_job_t *job = mr_job_create();
    int exit_code = 1;
    if (job == NULL) { fprintf(stderr, "out of memory\n"); return 1; }

    mr_error_t err = mr_job_set_map(job, mm_map, &dims);
    if (err == MR_OK) err = mr_job_set_reduce(job, mm_reduce, &dims);
    if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_LINES);
    if (err == MR_OK) err = mr_job_set_split_size(job, 256);
    if (err == MR_OK) err = mr_job_set_input(job, text, len);
    if (err == MR_OK) err = mr_job_run(job);
    if (err != MR_OK) {
        fprintf(stderr, "matrix_multiply failed: %s\n", mr_error_string(err));
        goto done;
    }

    /* Read the result cells back into C2 and compare with the plain triple loop. */
    long *C2 = calloc(n * p, sizeof(long));
    if (C2 == NULL) goto done;
    const mr_kv_list_t *out = mr_job_output(job);
    for (size_t r = 0; r < out->count; r++) {
        char *end;
        size_t i = strtoul(out->items[r].key, &end, 10);
        size_t k = strtoul(end + 1, NULL, 10);
        if (i < n && k < p) C2[i * p + k] = strtol(out->items[r].value, NULL, 10);
    }
    for (size_t i = 0; i < n; i++)
        for (size_t k = 0; k < p; k++)
            for (size_t j = 0; j < m; j++) C[i * p + k] += A[i * m + j] * B[j * p + k];

    printf("MATRIX MULTIPLICATION  A(%zux%zu) x B(%zux%zu)  (%zu mappers = one per split, %zu reducers)\n",
           n, m, m, p, mr_job_stats(job)->mappers_used, reducers);
    if (n <= 8 && m <= 8 && p <= 8) {
        printf("\nA:\n");
        for (size_t i = 0; i < n; i++) { for (size_t j = 0; j < m; j++) printf("%4ld", A[i * m + j]); printf("\n"); }
        printf("B:\n");
        for (size_t j = 0; j < m; j++) { for (size_t k = 0; k < p; k++) printf("%4ld", B[j * p + k]); printf("\n"); }
        printf("C = A x B (computed by MapReduce):\n");
        for (size_t i = 0; i < n; i++) { for (size_t k = 0; k < p; k++) printf("%5ld", C2[i * p + k]); printf("\n"); }
    }
    int same = (out->count == n * p) && memcmp(C, C2, n * p * sizeof(long)) == 0;
    printf("\nCheck: %zu cells from the job, compared with a plain triple loop -> %s\n",
           out->count, same ? "OK" : "MISMATCH");
    print_stats(job);
    exit_code = same ? 0 : 1;
    free(C2);

done:
    mr_job_destroy(job);
    free(text);
    free(A);
    free(B);
    free(C);
    return exit_code;
}
