/* test_partition.c - which partition does a key belong to? */
#include <stdio.h>
#include <string.h>

#include "mapreduce.h"
#include "test_util.h"

int main(void)
{
    TEST_SECTION("the hash is real 64-bit FNV-1a");
    /* FNV-1a("a") = 0xaf63dc4c8601ec8c = 12638187200555641996, which mod 1000 is 996.
     * FNV-1a("") is the offset basis 14695981039346656037, which mod 1000 is 37. */
    CHECK_EQ("hash(\"a\") mod 1000", 996, mr_partition_of("a", 1, 1000));
    CHECK_EQ("hash(\"\") mod 1000", 37, mr_partition_of("", 0, 1000));

    TEST_SECTION("same key, same partition");
    CHECK_EQ("\"the\" mod 3", 0, mr_partition_of("the", 3, 3));
    CHECK_EQ("\"the\" mod 8", 4, mr_partition_of("the", 3, 8));
    CHECK("the same key gives the same answer every time",
          mr_partition_of("hello", 5, 7) == mr_partition_of("hello", 5, 7));
    CHECK("only the first key_len bytes count",
          mr_partition_of("hello world", 5, 7) == mr_partition_of("hello", 5, 7));

    TEST_SECTION("the index is always in range");
    size_t sizes[] = { 1, 2, 3, 7, 16, 64 };
    int in_range = 1;
    for (int s = 0; s < 6; s++) {
        for (int k = 0; k < 2000; k++) {
            char key[16];
            int len = sprintf(key, "key%d", k);
            if (mr_partition_of(key, (size_t)len, sizes[s]) >= sizes[s]) in_range = 0;
        }
    }
    CHECK("2000 keys x 6 partition counts", in_range);
    CHECK_EQ("1 partition: everything goes to 0", 0, mr_partition_of("anything", 8, 1));

    TEST_SECTION("spread");
    size_t count[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < 10000; k++) {
        char key[16];
        int len = sprintf(key, "k%d", k);
        count[mr_partition_of(key, (size_t)len, 4)]++;
    }
    int balanced = 1;
    for (int p = 0; p < 4; p++)
        if (count[p] < 2000 || count[p] > 3000) balanced = 0;
    CHECK("10000 different keys spread roughly evenly over 4 partitions", balanced);

    TEST_SECTION("boundary cases");
    CHECK_EQ("0 partitions returns 0 (no crash)", 0, mr_partition_of("x", 1, 0));
    CHECK("keys with different length differ: \"a\\0b\" vs \"a\"",
          mr_partition_of("a\0b", 3, 1000) != mr_partition_of("a", 1, 1000));

    return TEST_FINISH();
}
