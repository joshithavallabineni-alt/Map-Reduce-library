/* test_kv.c - the key-value list: adding, copying, growing, sorting, freeing. */
#include <string.h>

#include "mapreduce.h"
#include "test_util.h"

int main(void)
{
    mr_kv_list_t list;

    TEST_SECTION("normal use");
    mr_kv_list_init(&list);
    CHECK_EQ("new list is empty", 0, list.count);
    CHECK_EQ("add (apple, 5)", MR_OK, mr_kv_list_add(&list, "apple", 5, "5", 1));
    CHECK_EQ("count is 1", 1, list.count);
    CHECK("key bytes are stored", list.items[0].key_len == 5 && memcmp(list.items[0].key, "apple", 5) == 0);
    CHECK("value bytes are stored", list.items[0].value_len == 1 && list.items[0].value[0] == '5');
    CHECK("an extra '\\0' follows the key (printable with %s)", strcmp(list.items[0].key, "apple") == 0);

    TEST_SECTION("the list keeps its own copy");
    char buffer[8] = "abc";
    mr_kv_list_add(&list, buffer, 3, "x", 1);
    buffer[0] = 'Z';
    CHECK("changing the caller's buffer does not change the pair", memcmp(list.items[1].key, "abc", 3) == 0);
    CHECK("the pair does not use the caller's pointer", list.items[1].key != buffer);
    mr_kv_list_free(&list);

    TEST_SECTION("boundary cases");
    mr_kv_list_init(&list);
    CHECK_EQ("empty value is allowed", MR_OK, mr_kv_list_add(&list, "k", 1, "", 0));
    CHECK_EQ("NULL value with length 0 is allowed", MR_OK, mr_kv_list_add(&list, "k2", 2, NULL, 0));
    CHECK_EQ("empty key is allowed", MR_OK, mr_kv_list_add(&list, "", 0, "v", 1));
    CHECK_EQ("key containing a '\\0' byte", MR_OK, mr_kv_list_add(&list, "a\0b", 3, "v", 1));
    CHECK_EQ("... keeps its full length", 3, list.items[3].key_len);
    CHECK("... and all three bytes", memcmp(list.items[3].key, "a\0b", 3) == 0);
    mr_kv_list_free(&list);

    TEST_SECTION("invalid arguments");
    mr_kv_list_init(&list);
    CHECK_EQ("NULL list", MR_ERR_NULL, mr_kv_list_add(NULL, "k", 1, "v", 1));
    CHECK_EQ("NULL key", MR_ERR_NULL, mr_kv_list_add(&list, NULL, 1, "v", 1));
    CHECK_EQ("NULL value with length > 0", MR_ERR_NULL, mr_kv_list_add(&list, "k", 1, NULL, 3));
    CHECK_EQ("the list is unchanged after errors", 0, list.count);
    mr_kv_list_init(NULL);
    CHECK("init(NULL) does not crash", 1);

    TEST_SECTION("growing: capacity 8, then doubling");
    mr_kv_list_add(&list, "a", 1, "1", 1);
    CHECK_EQ("capacity after the first add", 8, list.capacity);
    for (int i = 0; i < 8; i++) mr_kv_list_add(&list, "b", 1, "2", 1);
    CHECK_EQ("count is 9", 9, list.count);
    CHECK_EQ("capacity doubled to 16", 16, list.capacity);
    mr_kv_list_free(&list);

    TEST_SECTION("many pairs");
    mr_kv_list_init(&list);
    int all_ok = 1;
    for (int i = 0; i < 20000; i++) {
        char key[16];
        int n = sprintf(key, "key%d", i);
        if (mr_kv_list_add(&list, key, (size_t)n, "v", 1) != MR_OK) all_ok = 0;
    }
    CHECK("20000 adds succeeded", all_ok);
    CHECK_EQ("count is 20000", 20000, list.count);
    CHECK("last key is correct", strcmp(list.items[19999].key, "key19999") == 0);
    mr_kv_list_free(&list);

    TEST_SECTION("comparing and sorting");
    mr_kv_t a = { "apple", 5, "1", 1 }, b = { "banana", 6, "1", 1 }, c = { "app", 3, "9", 1 };
    mr_kv_t a2 = { "apple", 5, "2", 1 };
    CHECK("apple < banana", mr_kv_compare(&a, &b) < 0);
    CHECK("banana > apple", mr_kv_compare(&b, &a) > 0);
    CHECK("a prefix is smaller (app < apple)", mr_kv_compare(&c, &a) < 0);
    CHECK("same key: the value decides", mr_kv_compare(&a, &a2) < 0);
    CHECK_EQ("equal pairs compare as 0", 0, mr_kv_compare(&a, &a));

    mr_kv_list_init(&list);
    const char *words[] = { "pear", "apple", "fig", "apple", "banana" };
    for (int i = 0; i < 5; i++) mr_kv_list_add(&list, words[i], strlen(words[i]), i == 1 ? "2" : "1", 1);
    mr_kv_list_sort(&list);
    CHECK("sorted: apple, apple, banana, fig, pear",
          strcmp(list.items[0].key, "apple") == 0 && strcmp(list.items[1].key, "apple") == 0 &&
          strcmp(list.items[2].key, "banana") == 0 && strcmp(list.items[3].key, "fig") == 0 &&
          strcmp(list.items[4].key, "pear") == 0);
    CHECK("equal keys are ordered by value", list.items[0].value[0] == '1' && list.items[1].value[0] == '2');
    mr_kv_list_free(&list);
    mr_kv_list_sort(&list);   /* sorting an empty list */
    mr_kv_list_sort(NULL);
    CHECK("sorting an empty list or NULL does not crash", 1);

    TEST_SECTION("freeing");
    mr_kv_list_init(&list);
    mr_kv_list_add(&list, "k", 1, "v", 1);
    mr_kv_list_free(&list);
    CHECK_EQ("free resets the count", 0, list.count);
    CHECK("free resets the pointer", list.items == NULL);
    mr_kv_list_free(&list);
    mr_kv_list_free(NULL);
    CHECK("freeing twice and freeing NULL are safe", 1);

    TEST_SECTION("error texts");
    int texts_ok = 1;
    for (int e = MR_OK; e <= MR_ERR_THREAD; e++)
        if (mr_error_string((mr_error_t)e) == NULL || mr_error_string((mr_error_t)e)[0] == '\0') texts_ok = 0;
    CHECK("every error code has a text", texts_ok);
    CHECK("an unknown code has a text too", mr_error_string((mr_error_t)99) != NULL);

    return TEST_FINISH();
}
