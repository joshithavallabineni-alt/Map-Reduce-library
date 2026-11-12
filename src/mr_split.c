/* mr_split.c - input splitting.
 *
 * The input is cut into pieces of about split_size bytes. A split copies no
 * bytes: it only remembers where its piece starts and how long it is.
 *
 * Boundary alignment (this fixes the midterm limitation "a split can cut a
 * word"): in MR_SPLIT_WORDS / MR_SPLIT_LINES mode the cut is moved forward
 * until the byte before it is white space / '\n'. */
#include <stdlib.h>

#include "mapreduce.h"

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Is a cut allowed right after byte c? */
static int ends_piece(char c, mr_split_mode_t mode)
{
    if (mode == MR_SPLIT_LINES) return c == '\n';
    if (mode == MR_SPLIT_WORDS) return is_space(c);
    return 1;   /* MR_SPLIT_BYTES: anywhere */
}

mr_error_t mr_split_input(const char *input, size_t input_len,
                          size_t split_size, mr_split_mode_t mode,
                          mr_split_list_t *out)
{
    if (out == NULL) return MR_ERR_NULL;
    out->splits = NULL;                 /* always leave a valid empty list */
    out->count = 0;

    if (input == NULL && input_len > 0) return MR_ERR_NULL;
    if (split_size == 0) return MR_ERR_INVALID;
    if (mode != MR_SPLIT_BYTES && mode != MR_SPLIT_WORDS && mode != MR_SPLIT_LINES)
        return MR_ERR_INVALID;
    if (input_len == 0) return MR_OK;   /* nothing to split: 0 splits */

    /* At most ceil(input_len / split_size) splits (aligned cuts only make
     * splits longer, so there are never more). */
    size_t max_count = input_len / split_size;
    if (input_len % split_size != 0) max_count++;

    mr_split_t *splits = malloc(max_count * sizeof(mr_split_t));
    if (splits == NULL) return MR_ERR_NO_MEMORY;

    size_t count = 0;
    size_t start = 0;
    while (start < input_len) {
        size_t end = input_len - start > split_size ? start + split_size : input_len;
        /* Move the cut forward until it is allowed (never past the end). */
        while (end < input_len && !ends_piece(input[end - 1], mode)) end++;

        splits[count].data = input + start;
        splits[count].length = end - start;
        count++;
        start = end;
    }

    out->splits = splits;
    out->count = count;
    return MR_OK;
}

void mr_split_list_free(mr_split_list_t *list)
{
    if (list == NULL) return;
    free(list->splits);
    list->splits = NULL;
    list->count = 0;
}
