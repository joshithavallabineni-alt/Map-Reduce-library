/*
 * app_util.h - shared helpers for MapReduce examples.
 */

#ifndef APP_UTIL_H
#define APP_UTIL_H

#include <ctype.h>
#include <stdint.h>   /* SIZE_MAX, used by parse_positive */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapreduce.h"

/* Read an entire file into memory. */
__attribute__((unused))
static char *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;

    size_t cap = 1 << 16;
    size_t n = 0;

    char *buf = malloc(cap);

    while (buf != NULL) {
        if (n == cap) {
            char *bigger = realloc(buf, cap * 2);

            if (bigger == NULL) {
                free(buf);
                buf = NULL;
                break;
            }

            buf = bigger;
            cap *= 2;
        }

        size_t got = fread(buf + n, 1, cap - n, f);

        if (got == 0) break;

        n += got;
    }

    fclose(f);

    if (buf == NULL) return NULL;

    *out_len = n;
    return buf;
}

/*
 * Reads a non-negative number.
 *
 * Zero is allowed for automatic mapper allocation.
 * Reducer count is still checked separately by the application.
 */
__attribute__((unused))
static int parse_positive(const char *text, size_t *out)
{
    if (text == NULL || text[0] == '-' || text[0] == '\0')
        return 0;

    char *end = NULL;

    unsigned long long n = strtoull(text, &end, 10);

    if (end == text || *end != '\0')
        return 0;

    if (n > (unsigned long long)SIZE_MAX)
        return 0;

    *out = (size_t)n;

    return 1;
}

/* Read the next line. */
__attribute__((unused))
static int next_line(
    const char *s,
    size_t len,
    size_t *pos,
    const char **line,
    size_t *line_len)
{
    if (*pos >= len) return 0;

    size_t start = *pos;
    size_t i = start;

    while (i < len && s[i] != '\n')
        i++;

    *line = s + start;
    *line_len = i - start;

    *pos = (i < len) ? i + 1 : len;

    return 1;
}

/* Read the next whitespace-separated token. */
__attribute__((unused))
static int next_token(
    const char *s,
    size_t len,
    size_t *pos,
    const char **tok,
    size_t *tok_len)
{
    size_t i = *pos;

    while (i < len &&
           (s[i] == ' ' || s[i] == '\t' ||
            s[i] == '\r' || s[i] == '\n'))
        i++;

    if (i >= len) {
        *pos = len;
        return 0;
    }

    size_t start = i;

    while (i < len &&
           s[i] != ' ' && s[i] != '\t' &&
           s[i] != '\r' && s[i] != '\n')
        i++;

    *tok = s + start;
    *tok_len = i - start;
    *pos = i;

    return 1;
}

/*
 * WORD COUNT TOKENIZER
 *
 * Counts alphabetic words only.
 *
 * Examples:
 * hello       -> hello
 * HELLO       -> hello
 * 123         -> ignored
 * 000         -> ignored
 * hello123    -> ignored
 * abc45       -> ignored
 * 45world     -> ignored
 *
 * Punctuation separates words.
 */
__attribute__((unused))
static int next_word(
    const char *s,
    size_t len,
    size_t *pos,
    char *buf,
    size_t bufsize,
    size_t *word_len)
{
    size_t i = *pos;

    while (i < len) {

        /* Skip everything except letters and digits. */
        while (i < len &&
               !isalnum((unsigned char)s[i]))
            i++;

        if (i >= len) {
            *pos = len;
            return 0;
        }

        size_t n = 0;
        int has_digit = 0;
        int too_long = 0;

        /* Read one complete alphanumeric token. */
        while (i < len &&
               isalnum((unsigned char)s[i])) {

            unsigned char c = (unsigned char)s[i];

            if (isdigit(c))
                has_digit = 1;

            if (n + 1 < bufsize)
                buf[n++] = (char)tolower(c);
            else
                too_long = 1;

            i++;
        }

        *pos = i;

        /*
         * Ignore numeric tokens,
         * mixed alphanumeric tokens,
         * and tokens exceeding the buffer.
         */
        if (has_digit || too_long)
            continue;

        if (n == 0)
            continue;

        buf[n] = '\0';
        *word_len = n;

        return 1;
    }

    *pos = len;
    return 0;
}

/* Convert a token into a double. */
__attribute__((unused))
static double token_to_double(
    const char *tok,
    size_t len,
    int *ok)
{
    char buf[64];

    if (len == 0 || len >= sizeof(buf)) {
        *ok = 0;
        return 0.0;
    }

    memcpy(buf, tok, len);
    buf[len] = '\0';

    char *end = NULL;
    double v = strtod(buf, &end);

    *ok = (end != buf && *end == '\0');

    return v;
}

/* Print MapReduce job statistics. */
__attribute__((unused))
static void print_stats(const mr_job_t *job)
{
    const mr_job_stats_t *s = mr_job_stats(job);

    printf("\nJob statistics\n");

    printf("  splits: %zu   mappers (one per split): %zu   reducer threads used: %zu\n",
           s->num_splits,
           s->mappers_used,
           s->reducers_used);

    printf("  intermediate pairs: %zu   groups (distinct keys): %zu   output pairs: %zu\n",
           s->intermediate_pairs,
           s->groups,
           s->output_pairs);

    printf("  time (ms): split %.2f  map %.2f  shuffle %.2f  reduce %.2f  total %.2f\n",
           s->split_ms,
           s->map_ms,
           s->shuffle_ms,
           s->reduce_ms,
           s->total_ms);
}

#endif /* APP_UTIL_H */