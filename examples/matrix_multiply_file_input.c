#include "app_util.h"

typedef struct { size_t n, m, p; } dims_t;

static void mm_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    const dims_t *d = ctx;
    const char *line;
    size_t line_len, pos = 0;

    while (next_line(input, len, &pos, &line, &line_len)) {
        size_t pos_token = 0, t1, t2, t3, t4;
        const char *which, *a, *b, *val;

        if (!next_token(line, line_len, &pos_token, &which, &t1)) continue;
        if (!next_token(line, line_len, &pos_token, &a, &t2)) continue;
        if (!next_token(line, line_len, &pos_token, &b, &t3)) continue;
        if (!next_token(line, line_len, &pos_token, &val, &t4)) continue;
        if (t1 != 1 || t2 > 9 || t3 > 9 || t4 > 20) continue;

        char first[16], second[16], value[24];
        memcpy(first, a, t2); first[t2] = '\0';
        memcpy(second, b, t3); second[t3] = '\0';
        memcpy(value, val, t4); value[t4] = '\0';

        size_t x = strtoul(first, NULL, 10);
        size_t y = strtoul(second, NULL, 10);
        char key[40], v[64];

        if (which[0] == 'A') {
            /* A[i][j] contributes to every C[i][k]. */
            for (size_t k = 0; k < d->p; k++) {
                int kn = snprintf(key, sizeof(key), "%zu,%zu", x, k);
                int vn = snprintf(v, sizeof(v), "A,%zu,%s", y, value);
                mr_emit(out, key, (size_t)kn, v, (size_t)vn);
            }
        } else if (which[0] == 'B') {
            /* B[j][k] contributes to every C[i][k]. */
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
        const char *v = values[i].value; /* "A,j,value" or "B,j,value" */
        char *end;
        size_t j = strtoul(v + 2, &end, 10);
        long x = strtol(end + 1, NULL, 10);

        if (j >= d->m) continue;
        if (v[0] == 'A') a[j] = x;
        else if (v[0] == 'B') b[j] = x;
    }

    long sum = 0;
    for (size_t j = 0; j < d->m; j++) sum += a[j] * b[j];

    free(a);
    free(b);

    char result[32];
    int result_len = snprintf(result, sizeof(result), "%ld", sum);
    mr_emit(out, key, key_len, result, (size_t)result_len);
}

int main(int argc, char **argv)
{
    const char *input_path = (argc > 1) ? argv[1] : "matrix_input.txt";
    size_t mappers = 2, reducers = 2;

    if (argc > 2 && !parse_positive(argv[2], &mappers)) {
        fprintf(stderr, "Invalid mapper count\n");
        return 2;
    }
    if (argc > 3 && !parse_positive(argv[3], &reducers)) {
        fprintf(stderr, "Invalid reducer count\n");
        return 2;
    }

    /* Open the user-provided matrix input file. */
    FILE *fp = fopen(input_path, "r");
    if (fp == NULL) {
        perror("Cannot open matrix input file");
        return 1;
    }

    size_t n, m, rows_B, p;
    if (fscanf(fp, "%zu %zu", &n, &m) != 2 || n == 0 || m == 0) {
        fprintf(stderr, "Invalid dimensions for matrix A\n");
        fclose(fp);
        return 1;
    }

    if (n > 100000 || m > 100000 || n * m > 4000000) {
        fprintf(stderr, "Matrix A too large for this example\n");
        fclose(fp);
        return 2;
    }

    long *A = malloc(n * m * sizeof(long));
    if (A == NULL) {
        fprintf(stderr, "Out of memory for A\n");
        fclose(fp);
        return 1;
    }

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < m; j++) {
            if (fscanf(fp, "%ld", &A[i * m + j]) != 1) {
                fprintf(stderr, "Not enough/invalid values in matrix A\n");
                fclose(fp);
                free(A);
                return 1;
            }
        }
    }

    if (fscanf(fp, "%zu %zu", &rows_B, &p) != 2 || rows_B != m || p == 0) {
        fprintf(stderr, "Invalid dimensions for B: B rows must equal A columns (%zu)\n", m);
        fclose(fp);
        free(A);
        return 1;
    }

    if (p > 100000 || m * p > 4000000 || n * p > 4000000) {
        fprintf(stderr, "Matrix B/result too large for this example\n");
        fclose(fp);
        free(A);
        return 2;
    }

    long *B = malloc(m * p * sizeof(long));
    long *C = calloc(n * p, sizeof(long));
    if (B == NULL || C == NULL) {
        fprintf(stderr, "Out of memory for B/C\n");
        fclose(fp);
        free(A);
        free(B);
        free(C);
        return 1;
    }

    for (size_t j = 0; j < m; j++) {
        for (size_t k = 0; k < p; k++) {
            if (fscanf(fp, "%ld", &B[j * p + k]) != 1) {
                fprintf(stderr, "Not enough/invalid values in matrix B\n");
                fclose(fp);
                free(A);
                free(B);
                free(C);
                return 1;
            }
        }
    }
    fclose(fp);

    /* Convert the matrices into records understood by the existing mapper. */
    size_t records = n * m + m * p;
    size_t cap = records * 64 + 1;
    size_t len = 0;
    char *text = malloc(cap);
    if (text == NULL) {
        fprintf(stderr, "Out of memory for MapReduce input\n");
        free(A); free(B); free(C);
        return 1;
    }

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < m; j++) {
            int written = snprintf(text + len, cap - len,
                                   "A %zu %zu %ld\n", i, j, A[i * m + j]);
            if (written < 0 || (size_t)written >= cap - len) {
                fprintf(stderr, "Input buffer overflow\n");
                free(text); free(A); free(B); free(C);
                return 1;
            }
            len += (size_t)written;
        }
    }

    for (size_t j = 0; j < m; j++) {
        for (size_t k = 0; k < p; k++) {
            int written = snprintf(text + len, cap - len,
                                   "B %zu %zu %ld\n", j, k, B[j * p + k]);
            if (written < 0 || (size_t)written >= cap - len) {
                fprintf(stderr, "Input buffer overflow\n");
                free(text); free(A); free(B); free(C);
                return 1;
            }
            len += (size_t)written;
        }
    }

    dims_t dims = { n, m, p };
    mr_job_t *job = mr_job_create();
    if (job == NULL) {
        fprintf(stderr, "Could not create MapReduce job\n");
        free(text); free(A); free(B); free(C);
        return 1;
    }

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
        mr_job_destroy(job);
        free(text); free(A); free(B); free(C);
        return 1;
    }

    /* Read MapReduce output into C2. */
    long *C2 = calloc(n * p, sizeof(long));
    if (C2 == NULL) {
        fprintf(stderr, "Out of memory for result matrix\n");
        mr_job_destroy(job);
        free(text); free(A); free(B); free(C);
        return 1;
    }

    const mr_kv_list_t *out = mr_job_output(job);
    for (size_t r = 0; r < out->count; r++) {
        char *end;
        size_t i = strtoul(out->items[r].key, &end, 10);
        size_t k = strtoul(end + 1, NULL, 10);
        if (i < n && k < p)
            C2[i * p + k] = strtol(out->items[r].value, NULL, 10);
    }

    /* Sequential multiplication used only to verify MapReduce output. */
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < p; k++) {
            for (size_t j = 0; j < m; j++)
                C[i * p + k] += A[i * m + j] * B[j * p + k];
        }
    }

    /* Save the result matrix to a text file. */
    FILE *out_fp = fopen("matrix_output.txt", "w");
    if (out_fp == NULL) {
        perror("Cannot write matrix_output.txt");
    } else {
        fprintf(out_fp, "%zu %zu\n", n, p);
        for (size_t i = 0; i < n; i++) {
            for (size_t k = 0; k < p; k++)
                fprintf(out_fp, "%ld%s", C2[i * p + k], (k + 1 == p) ? "" : " ");
            fprintf(out_fp, "\n");
        }
        fclose(out_fp);
    }

    printf("Matrix multiplication: A(%zux%zu) x B(%zux%zu) -> C(%zux%zu)\n",
           n, m, m, p, n, p);
    printf("Mappers: %zu, reducers: %zu\n", mappers, reducers);
    printf("Result saved to matrix_output.txt\n");

    if (n <= 10 && p <= 10) {
        printf("\nC = A x B:\n");
        for (size_t i = 0; i < n; i++) {
            for (size_t k = 0; k < p; k++)
                printf("%ld%s", C2[i * p + k], (k + 1 == p) ? "" : "\t");
            printf("\n");
        }
    }

    int same = (out->count == n * p) &&
               memcmp(C, C2, n * p * sizeof(long)) == 0;
    printf("\nCorrectness check (MapReduce vs sequential): %s\n",
           same ? "OK" : "MISMATCH");
    print_stats(job);

    mr_job_destroy(job);
    free(C2);
    free(text);
    free(A);
    free(B);
    free(C);
    return same ? 0 : 1;
}
