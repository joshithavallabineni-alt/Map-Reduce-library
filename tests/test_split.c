/* test_split.c - input splitting: byte, word and line mode. */
#include <stdlib.h>
#include <string.h>

#include "mapreduce.h"
#include "test_util.h"

/* Do the splits cover the input exactly once, in order, with no gaps? */
static int covers_input(const mr_split_list_t *l, const char *input, size_t len)
{
    size_t pos = 0;
    for (size_t i = 0; i < l->count; i++) {
        if (l->splits[i].data != input + pos) return 0;
        if (l->splits[i].length == 0) return 0;
        pos += l->splits[i].length;
    }
    return pos == len;
}

int main(void)
{
    mr_split_list_t sp;

    TEST_SECTION("byte mode: exactly split_size bytes");
    const char *ten = "0123456789";
    CHECK_EQ("status", MR_OK, mr_split_input(ten, 10, 4, MR_SPLIT_BYTES, &sp));
    CHECK_EQ("10 bytes / size 4 = 3 splits", 3, sp.count);
    CHECK("lengths are 4, 4, 2", sp.splits[0].length == 4 && sp.splits[1].length == 4 && sp.splits[2].length == 2);
    CHECK("splits point into the input (no copy)", sp.splits[1].data == ten + 4);
    CHECK("the splits cover the input", covers_input(&sp, ten, 10));
    mr_split_list_free(&sp);

    CHECK_EQ("exact multiple: 8 / 4", MR_OK, mr_split_input(ten, 8, 4, MR_SPLIT_BYTES, &sp));
    CHECK_EQ("... gives 2 splits", 2, sp.count);
    mr_split_list_free(&sp);
    mr_split_input(ten, 10, 100, MR_SPLIT_BYTES, &sp);
    CHECK_EQ("split size larger than the input: 1 split", 1, sp.count);
    mr_split_list_free(&sp);
    mr_split_input(ten, 1, 1, MR_SPLIT_BYTES, &sp);
    CHECK_EQ("1 byte, size 1: 1 split", 1, sp.count);
    mr_split_list_free(&sp);

    TEST_SECTION("word mode: no word is ever cut");
    const char *text = "the cat sat dog the mat";      /* 23 bytes */
    CHECK_EQ("status", MR_OK, mr_split_input(text, strlen(text), 5, MR_SPLIT_WORDS, &sp));
    CHECK("the splits cover the input", covers_input(&sp, text, strlen(text)));
    int cut = 0;                                        /* every split except the last must end after a space */
    for (size_t i = 0; i + 1 < sp.count; i++)
        if (sp.splits[i].data[sp.splits[i].length - 1] != ' ') cut = 1;
    CHECK("every split ends right after a space", !cut);
    CHECK("the first split is \"the cat \" (cut moved forward from 5 to 8)",
          sp.splits[0].length == 8 && memcmp(sp.splits[0].data, "the cat ", 8) == 0);
    mr_split_list_free(&sp);

    mr_split_input("abcdefghij", 10, 3, MR_SPLIT_WORDS, &sp);
    CHECK_EQ("a single long word: 1 split (nowhere to cut)", 1, sp.count);
    CHECK_EQ("... holding all 10 bytes", 10, sp.splits[0].length);
    mr_split_list_free(&sp);

    TEST_SECTION("line mode: splits hold whole lines");
    const char *lines = "aaaa\nbb\ncccccc\nd\n";       /* 17 bytes */
    CHECK_EQ("status", MR_OK, mr_split_input(lines, strlen(lines), 6, MR_SPLIT_LINES, &sp));
    CHECK("the splits cover the input", covers_input(&sp, lines, strlen(lines)));
    CHECK("split 0 is \"aaaa\\nbb\\n\"", sp.splits[0].length == 8 && memcmp(sp.splits[0].data, "aaaa\nbb\n", 8) == 0);
    int line_ok = 1;
    for (size_t i = 0; i < sp.count; i++)
        if (sp.splits[i].data[sp.splits[i].length - 1] != '\n') line_ok = 0;
    CHECK("every split ends with a newline", line_ok);
    mr_split_list_free(&sp);

    mr_split_input("no newline at all", 17, 4, MR_SPLIT_LINES, &sp);
    CHECK_EQ("no newline in the input: 1 split", 1, sp.count);
    mr_split_list_free(&sp);

    TEST_SECTION("empty input");
    CHECK_EQ("empty input is not an error", MR_OK, mr_split_input("", 0, 4, MR_SPLIT_BYTES, &sp));
    CHECK_EQ("0 splits", 0, sp.count);
    CHECK_EQ("NULL input with length 0 is also empty", MR_OK, mr_split_input(NULL, 0, 4, MR_SPLIT_LINES, &sp));
    CHECK_EQ("0 splits", 0, sp.count);
    mr_split_list_free(&sp);

    TEST_SECTION("invalid input");
    CHECK_EQ("split size 0", MR_ERR_INVALID, mr_split_input(ten, 10, 0, MR_SPLIT_BYTES, &sp));
    CHECK("the result is a valid empty list", sp.count == 0 && sp.splits == NULL);
    CHECK_EQ("unknown mode", MR_ERR_INVALID, mr_split_input(ten, 10, 4, (mr_split_mode_t)42, &sp));
    CHECK_EQ("NULL input with length > 0", MR_ERR_NULL, mr_split_input(NULL, 5, 4, MR_SPLIT_BYTES, &sp));
    CHECK_EQ("NULL output", MR_ERR_NULL, mr_split_input(ten, 10, 4, MR_SPLIT_BYTES, NULL));
    mr_split_list_free(NULL);
    mr_split_list_free(&sp);
    mr_split_list_free(&sp);
    CHECK("freeing NULL and freeing twice are safe", 1);

    TEST_SECTION("large input: 20 MB in 4 KB splits, every mode");
    size_t big_len = 20u * 1024 * 1024;
    char *big = malloc(big_len);
    if (big == NULL) {
        CHECK("could not allocate memory for this test", 0);
    } else {
        for (size_t i = 0; i < big_len; i++) big[i] = (i % 17 == 16) ? '\n' : (i % 5 == 4 ? ' ' : 'x');
        for (int mode = 0; mode < 3; mode++) {
            CHECK_EQ("status", MR_OK, mr_split_input(big, big_len, 4096, (mr_split_mode_t)mode, &sp));
            CHECK("splits cover the input exactly", covers_input(&sp, big, big_len));
            CHECK("about 5000 splits", sp.count > 4000 && sp.count <= 5121);
            mr_split_list_free(&sp);
        }
        free(big);
    }

    return TEST_FINISH();
}
