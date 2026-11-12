/* mr_partition.c - which partition (reducer) does a key belong to?
 *
 * partition = hash(key) mod num_partitions. The hash depends only on the key
 * bytes, so all pairs with the same key always end up in the same partition,
 * whichever mapper emitted them. */
#include <stdint.h>

#include "mapreduce.h"

/* FNV-1a, a very small hash: start with a constant, then for each byte
 * XOR it in and multiply by a prime. */
static uint64_t fnv1a_hash(const char *data, size_t len)
{
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

size_t mr_partition_of(const char *key, size_t key_len, size_t num_partitions)
{
    if (num_partitions == 0) return 0;
    return (size_t)(fnv1a_hash(key, key_len) % num_partitions);
}
