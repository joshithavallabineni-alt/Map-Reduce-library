/* mr_kv.c - the key-value list.
 *
 * Memory rule: every pair owns two heap buffers (key and value). They are
 * allocated in mr_kv_list_add and freed in mr_kv_list_free. (Inside the
 * library the shuffle phase MOVES pairs between lists instead of copying;
 * it always frees the source array afterwards so nothing is freed twice.) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mapreduce.h"

#define FIRST_CAPACITY 8

void mr_kv_list_init(mr_kv_list_t *list)
{
    if (list == NULL) return;
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

mr_error_t mr_kv_list_add(mr_kv_list_t *list,
                          const char *key, size_t key_len,
                          const char *value, size_t value_len)
{
    if (list == NULL || key == NULL) return MR_ERR_NULL;
    if (value == NULL && value_len > 0) return MR_ERR_NULL;

    /* 1. Make room for one more pair (double the array when it is full). */
    if (list->count == list->capacity) {
        size_t new_capacity = FIRST_CAPACITY;
        if (list->capacity > 0) new_capacity = list->capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(mr_kv_t)) return MR_ERR_NO_MEMORY;
        mr_kv_t *bigger = realloc(list->items, new_capacity * sizeof(mr_kv_t));
        if (bigger == NULL) return MR_ERR_NO_MEMORY;   /* list unchanged */
        list->items = bigger;
        list->capacity = new_capacity;
    }

    /* 2. Copy key and value. The "+ 1" holds a '\0' so the copies can be
     *    printed as strings (and malloc is never asked for 0 bytes). */
    if (key_len == SIZE_MAX || value_len == SIZE_MAX) return MR_ERR_NO_MEMORY;
    char *key_copy = malloc(key_len + 1);
    if (key_copy == NULL) return MR_ERR_NO_MEMORY;
    char *value_copy = malloc(value_len + 1);
    if (value_copy == NULL) {
        free(key_copy);
        return MR_ERR_NO_MEMORY;
    }
    memcpy(key_copy, key, key_len);
    key_copy[key_len] = '\0';
    if (value_len > 0) memcpy(value_copy, value, value_len);
    value_copy[value_len] = '\0';

    /* 3. Store the pair. */
    mr_kv_t *pair = &list->items[list->count];
    pair->key = key_copy;
    pair->key_len = key_len;
    pair->value = value_copy;
    pair->value_len = value_len;
    list->count++;
    return MR_OK;
}

void mr_kv_list_free(mr_kv_list_t *list)
{
    if (list == NULL) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].key);
        free(list->items[i].value);
    }
    free(list->items);
    mr_kv_list_init(list);   /* so a second free does nothing */
}

/* Compares two byte buffers of known length. */
static int compare_bytes(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t common = a_len < b_len ? a_len : b_len;
    int c = common > 0 ? memcmp(a, b, common) : 0;
    if (c != 0) return c;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

int mr_kv_compare(const mr_kv_t *a, const mr_kv_t *b)
{
    int c = compare_bytes(a->key, a->key_len, b->key, b->key_len);
    if (c != 0) return c;
    return compare_bytes(a->value, a->value_len, b->value, b->value_len);
}

static int qsort_compare(const void *a, const void *b)
{
    return mr_kv_compare((const mr_kv_t *)a, (const mr_kv_t *)b);
}

void mr_kv_list_sort(mr_kv_list_t *list)
{
    if (list == NULL || list->count < 2) return;
    qsort(list->items, list->count, sizeof(mr_kv_t), qsort_compare);
}
