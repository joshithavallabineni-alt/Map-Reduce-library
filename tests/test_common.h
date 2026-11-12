/*
 * test_common.h - word-count helpers shared by several test programs.
 * (Not part of the library.)
 */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapreduce.h"

#define UNUSED __attribute__((unused))

/* map: emit (word, "1") for every white-space separated word */
UNUSED static void wc_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    size_t i = 0;
    while (i < len) {
        while (i < len && (input[i] == ' ' || input[i] == '\n' || input[i] == '\t')) i++;
        size_t start = i;
        while (i < len && input[i] != ' ' && input[i] != '\n' && input[i] != '\t') i++;
        if (i > start) mr_emit(out, input + start, i - start, "1", 1);
    }
}

/* reduce: add up the values, emit (word, total) */
UNUSED static void wc_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                             size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    unsigned long total = 0;
    for (size_t i = 0; i < num_values; i++) total += strtoul(values[i].value, NULL, 10);
    char text[32];
    int n = snprintf(text, sizeof(text), "%lu", total);
    mr_emit(out, key, key_len, text, (size_t)n);
}

/* Runs word count and returns the job (NULL if something failed).
 * The caller destroys the job. */
UNUSED static mr_job_t *run_wordcount(const char *text, size_t len, size_t mappers,
                                      size_t reducers, size_t split_size,
                                      mr_split_mode_t mode, mr_error_t *err_out)
{
    mr_job_t *job = mr_job_create();
    mr_error_t err = (job == NULL) ? MR_ERR_NO_MEMORY : MR_OK;
    if (err == MR_OK) err = mr_job_set_map(job, wc_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job, wc_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job, mode);
    if (err == MR_OK) err = mr_job_set_split_size(job, split_size);
    if (err == MR_OK) err = mr_job_set_input(job, text, len);
    if (err == MR_OK) err = mr_job_run(job);
    if (err_out != NULL) *err_out = err;
    if (err != MR_OK) {
        mr_job_destroy(job);
        return NULL;
    }
    return job;
}

/* The whole output as one string "key=value;key=value;" (free it). */
UNUSED static char *output_string(const mr_job_t *job)
{
    const mr_kv_list_t *out = mr_job_output(job);
    size_t need = 1;
    for (size_t i = 0; i < out->count; i++) need += out->items[i].key_len + out->items[i].value_len + 2;
    char *s = malloc(need);
    if (s == NULL) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < out->count; i++) {
        memcpy(s + n, out->items[i].key, out->items[i].key_len);
        n += out->items[i].key_len;
        s[n++] = '=';
        memcpy(s + n, out->items[i].value, out->items[i].value_len);
        n += out->items[i].value_len;
        s[n++] = ';';
    }
    s[n] = '\0';
    return s;
}

/* Value stored for a key, or -1 if the key is not in the output. */
UNUSED static long output_value(const mr_job_t *job, const char *key)
{
    const mr_kv_list_t *out = mr_job_output(job);
    for (size_t i = 0; i < out->count; i++) {
        if (out->items[i].key_len == strlen(key) && memcmp(out->items[i].key, key, out->items[i].key_len) == 0)
            return strtol(out->items[i].value, NULL, 10);
    }
    return -1;
}

/* A repeatable text of `words` words (50 different words, 10 per line). */
UNUSED static char *make_text(size_t words, size_t *out_len)
{
    char *text = malloc(words * 8 + 8);
    if (text == NULL) return NULL;
    size_t len = 0;
    for (size_t i = 0; i < words; i++) {
        unsigned id = (unsigned)((i * 7 + i / 3) % 50);
        len += (size_t)sprintf(text + len, "w%u%c", id, (i % 10 == 9) ? '\n' : ' ');
    }
    text[len] = '\0';
    *out_len = len;
    return text;
}

#endif /* TEST_COMMON_H */
