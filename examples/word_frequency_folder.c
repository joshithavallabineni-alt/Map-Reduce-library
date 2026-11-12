/*
 * word_frequency_folder.c - application 4 for FOLDERS: WORD FREQUENCY ANALYSIS
 *                           over many .txt files (two chained jobs)
 *
 * The same analysis as word_frequency.c, with a FOLDER as input:
 * Job 1 (word count over all files):
 *     input  = the list of file paths, one per line; one path per split, so ONE
 *              MAPPER PER FILE; the mapper reads its file itself
 *     map    : count the words of that file, emit (word, count in file)
 *     reduce : add up -> (word, how many times in ALL files)
 * Job 2 (frequency of frequencies):
 *     input = the output of job 1, one line per word:  "word<TAB>count"
 *     map    : emit (count, "1")
 *     reduce : add up -> (count, how many different words occur that often)
 * Then the program prints the most frequent words with their share of all
 * words, and the histogram from job 2.
 *
 * Usage: ./examples/word_frequency_folder <folder> [max-mappers] [reducers] [-o output-file]
 *   -o writes ALL words ("word<TAB>count" lines, the output of job 1) to a file.
 */
#define _POSIX_C_SOURCE 200809L          /* for opendir/readdir/stat (POSIX, not plain C11) */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "app_util.h"

/* ---- folder helpers: the SAME code is in word_count_folder.c,
 *      word_frequency_folder.c and inverted_index_folder.c (examples are single
 *      files on purpose, and app_util.h is left unchanged) ---------------------- */

#define MAX_PATH_LEN 4096

/* The usable .txt files of a folder. "paths" is the INPUT of the MapReduce job:
 * one path per line. Every line becomes its own split, so every file gets its
 * own mapper, and the mapper opens and reads that file itself. This way the
 * text of all files is never held in memory at the same time, and every mapper
 * knows which file (= which document) it is reading. */
typedef struct {
    char  *paths;                        /* "folder/name\n" for every usable file  */
    size_t paths_len;
    size_t num_files;                    /* usable files = number of mappers        */
    size_t empty, unreadable, ignored;   /* files that were skipped                 */
} filelist_t;

static int has_txt_extension(const char *name)      /* ".txt" or ".TXT", at least 1 more letter */
{
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char *e = name + n - 4;
    return e[0] == '.' && tolower((unsigned char)e[1]) == 't' &&
           tolower((unsigned char)e[2]) == 'x' && tolower((unsigned char)e[3]) == 't';
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Lists the .txt files directly inside folder (not in sub-folders), sorted by name.
 * Skipped with a counter: empty files, files that cannot be opened (a warning is
 * printed), things that are not regular files, names that cannot be used.
 * Returns 0 on success, -1 on an error (the message is already printed). */
static int scan_folder(const char *folder, filelist_t *fl)
{
    memset(fl, 0, sizeof(*fl));
    DIR *dir = opendir(folder);
    if (dir == NULL) {
        fprintf(stderr, "cannot open folder \"%s\": %s\n", folder, strerror(errno));
        return -1;
    }
    size_t folder_len = strlen(folder);
    int need_slash = (folder_len > 0 && folder[folder_len - 1] != '/');
    char **list = NULL;
    size_t count = 0, capacity = 0;
    int failed = 0;

    struct dirent *entry;
    while (!failed && (entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!has_txt_extension(name)) continue;
        size_t path_len = folder_len + (size_t)need_slash + strlen(name);
        if (path_len >= MAX_PATH_LEN || strchr(name, '\n') != NULL) {
            fprintf(stderr, "warning: skipped \"%s\" (name too long or contains a newline)\n", name);
            fl->ignored++;
            continue;
        }
        char *path = malloc(path_len + 1);
        if (path == NULL) { failed = 1; break; }
        snprintf(path, path_len + 1, "%s%s%s", folder, need_slash ? "/" : "", name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { fl->ignored++; free(path); continue; }
        if (st.st_size == 0) { fl->empty++; free(path); continue; }
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            fprintf(stderr, "warning: cannot open \"%s\": %s (skipped)\n", path, strerror(errno));
            fl->unreadable++;
            free(path);
            continue;
        }
        fclose(f);

        if (count == capacity) {
            size_t bigger_cap = capacity ? capacity * 2 : 256;
            char **bigger = realloc(list, bigger_cap * sizeof(char *));
            if (bigger == NULL) { free(path); failed = 1; break; }
            list = bigger;
            capacity = bigger_cap;
        }
        list[count++] = path;
    }
    closedir(dir);

    if (!failed && count > 0) {
        qsort(list, count, sizeof(char *), cmp_path);      /* same order on every run */
        size_t need = 1;
        for (size_t i = 0; i < count; i++) need += strlen(list[i]) + 1;
        fl->paths = malloc(need);
        if (fl->paths == NULL) {
            failed = 1;
        } else {
            for (size_t i = 0; i < count; i++) {
                size_t n = strlen(list[i]);
                memcpy(fl->paths + fl->paths_len, list[i], n);
                fl->paths_len += n;
                fl->paths[fl->paths_len++] = '\n';
            }
            fl->num_files = count;
        }
    }
    for (size_t i = 0; i < count; i++) free(list[i]);
    free(list);
    if (failed) {
        fprintf(stderr, "out of memory while listing the folder\n");
        free(fl->paths);
        memset(fl, 0, sizeof(*fl));
        return -1;
    }
    return 0;
}

static void print_folder_summary(const char *folder, const filelist_t *fl)
{
    printf("Folder \"%s\": %zu .txt file(s) processed", folder, fl->num_files);
    if (fl->empty) printf(", %zu empty skipped", fl->empty);
    if (fl->unreadable) printf(", %zu unreadable skipped", fl->unreadable);
    if (fl->ignored) printf(", %zu other skipped", fl->ignored);
    printf("\n");
}

/* ---- the words of ONE file: a small hash table (word -> count) ----------- */
/* Counting inside the mapper first ("in-mapper combining") means a word that
 * occurs 1000 times in a file is emitted once, not 1000 times: far fewer
 * pairs to copy, shuffle and sort on a large data set. The result is the same. */
typedef struct { char *word; size_t len; unsigned long count; } slot_t;
typedef struct { slot_t *slots; size_t size, used; } wordtab_t;      /* size = power of 2 */

static size_t hash_word(const char *w, size_t len)               /* FNV-1a */
{
    unsigned long long h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)w[i]; h *= 1099511628211ULL; }
    return (size_t)h;
}

static int tab_init(wordtab_t *t)
{
    t->size = 1024;
    t->used = 0;
    t->slots = calloc(t->size, sizeof(slot_t));
    return t->slots != NULL ? 0 : -1;
}

static void tab_free(wordtab_t *t)
{
    if (t->slots != NULL)
        for (size_t i = 0; i < t->size; i++) free(t->slots[i].word);
    free(t->slots);
    t->slots = NULL;
    t->size = t->used = 0;
}

static slot_t *tab_slot(slot_t *slots, size_t size, const char *w, size_t len)
{
    size_t i = hash_word(w, len) & (size - 1);
    while (slots[i].word != NULL && !(slots[i].len == len && memcmp(slots[i].word, w, len) == 0))
        i = (i + 1) & (size - 1);
    return &slots[i];
}

static int tab_add(wordtab_t *t, const char *w, size_t len)
{
    if ((t->used + 1) * 2 > t->size) {                            /* keep it half empty */
        size_t new_size = t->size * 2;
        slot_t *bigger = calloc(new_size, sizeof(slot_t));
        if (bigger == NULL) return -1;
        for (size_t i = 0; i < t->size; i++)
            if (t->slots[i].word != NULL)
                *tab_slot(bigger, new_size, t->slots[i].word, t->slots[i].len) = t->slots[i];
        free(t->slots);
        t->slots = bigger;
        t->size = new_size;
    }
    slot_t *s = tab_slot(t->slots, t->size, w, len);
    if (s->word == NULL) {
        s->word = malloc(len + 1);
        if (s->word == NULL) return -1;
        memcpy(s->word, w, len);
        s->word[len] = '\0';
        s->len = len;
        s->count = 0;
        t->used++;
    }
    s->count++;
    return 0;
}

/* Reads one file and adds its words (letters/digits, lower case, same rule as
 * the single-file examples) to the table. Returns 0, or -1 if the file cannot
 * be read or memory is short. */
static int count_file(const char *path, wordtab_t *t)
{
    size_t len = 0;
    char *text = read_whole_file(path, &len);
    if (text == NULL) return -1;
    char word[128];
    size_t word_len, pos = 0;
    int rc = 0;
    while (rc == 0 && next_word(text, len, &pos, word, sizeof(word), &word_len))
        rc = tab_add(t, word, word_len);
    free(text);
    return rc;
}

/* Copies the next line of the job input (= one file path) into path[]. */
static int next_path(const char *input, size_t len, size_t *pos, char *path)
{
    const char *line;
    size_t line_len;
    while (next_line(input, len, pos, &line, &line_len)) {
        if (line_len == 0 || line_len >= MAX_PATH_LEN) continue;
        memcpy(path, line, line_len);
        path[line_len] = '\0';
        return 1;
    }
    return 0;
}

/* Independent check: all words of one file in a single pass. */
__attribute__((unused)) static unsigned long count_words(const char *text, size_t len)
{
    char word[128];
    size_t word_len, pos = 0;
    unsigned long n = 0;
    while (next_word(text, len, &pos, word, sizeof(word), &word_len)) n++;
    return n;
}

/* ---- job 1: word count over the files (its input is a list of paths) --------- */
static void count_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    char path[MAX_PATH_LEN];
    size_t pos = 0;
    while (next_path(input, len, &pos, path)) {
        wordtab_t tab;
        if (tab_init(&tab) != 0) { fprintf(stderr, "out of memory\n"); mr_emit_fail(out); return; }
        if (count_file(path, &tab) != 0) {
            fprintf(stderr, "cannot read \"%s\"\n", path);
            tab_free(&tab);
            mr_emit_fail(out);
            return;
        }
        for (size_t i = 0; i < tab.size; i++) {
            if (tab.slots[i].word == NULL) continue;
            char text[32];
            int n = snprintf(text, sizeof(text), "%lu", tab.slots[i].count);
            mr_emit(out, tab.slots[i].word, tab.slots[i].len, text, (size_t)n);
        }
        tab_free(&tab);
    }
}

static void count_reduce(const char *key, size_t key_len, const mr_kv_t *values,
                         size_t num_values, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    unsigned long total = 0;
    for (size_t i = 0; i < num_values; i++) total += strtoul(values[i].value, NULL, 10);
    char text[32];
    int n = snprintf(text, sizeof(text), "%lu", total);
    mr_emit(out, key, key_len, text, (size_t)n);
}

/* ---- job 2: how many words have each count (same as word_frequency.c) -------- */
static void hist_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    const char *line;
    size_t line_len, pos = 0;
    while (next_line(input, len, &pos, &line, &line_len)) {
        size_t t = 0;
        while (t < line_len && line[t] != '\t') t++;           /* skip the word */
        if (t >= line_len) continue;
        unsigned long count = 0;
        for (size_t i = t + 1; i < line_len && isdigit((unsigned char)line[i]); i++)
            count = count * 10 + (unsigned long)(line[i] - '0');
        char key[32];
        int n = snprintf(key, sizeof(key), "%010lu", count);   /* zero-padded: sorts as a number */
        mr_emit(out, key, (size_t)n, "1", 1);
    }
}

/* ---- sorting the job-1 output for printing ------------------------- */
typedef struct { const char *word; unsigned long count; } entry_t;

static int by_count_desc(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;
    return strcmp(x->word, y->word);
}

/* Turns the word typed after -w into the form stored in the table:
 * lower case, letters only (the tokenizer ignores digits). 0 = ok, -1 = bad. */
static int normalize_query(const char *q, char *buf, size_t size)
{
    size_t n = strlen(q);
    if (n == 0 || n + 1 > size) return -1;
    for (size_t i = 0; i < n; i++) {
        if (!isalpha((unsigned char)q[i])) return -1;
        buf[i] = (char)tolower((unsigned char)q[i]);
    }
    buf[n] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    const char *usage = "usage: %s <folder> [max-mappers] [reducers] [-w word] [-o output-file]\n"
                        "  max-mappers 0 = automatic (one mapper per file)\n"
                        "  -w word     show the analysis of that one word only (default: all words)\n"
                        "  -o file     write ALL words to a file\n";
    const char *folder = NULL, *out_file = NULL, *query = NULL;
    size_t mappers = 0, reducers = 2;     /* mappers 0 = one per split */
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out_file = argv[++i]; }
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) { query = argv[++i]; }
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "unknown argument \"%s\"\n", argv[i]);
            fprintf(stderr, usage, argv[0]);
            return 2;
        }
        else if (npos == 0) { folder = argv[i]; npos++; }
        else if (npos == 1) {
            if (!parse_positive(argv[i], &mappers)) { fprintf(stderr, "bad mappers \"%s\"\n", argv[i]); return 2; }
            npos++;
        }
        else if (npos == 2) {
            if (!parse_positive(argv[i], &reducers)) { fprintf(stderr, "bad reducers \"%s\"\n", argv[i]); return 2; }
            npos++;
        }
        else { fprintf(stderr, "unknown argument \"%s\"\n", argv[i]); return 2; }
    }
    if (folder == NULL) { fprintf(stderr, usage, argv[0]); return 2; }
    if (reducers == 0 || reducers > MR_MAX_WORKERS) {
        fprintf(stderr, "reducers must be between 1 and %d\n", MR_MAX_WORKERS);
        return 2;
    }
    if (mappers > MR_MAX_WORKERS) {
        fprintf(stderr, "max-mappers must be between 0 (automatic) and %d\n", MR_MAX_WORKERS);
        return 2;
    }
    char qword[128];
    size_t qlen = 0;
    if (query != NULL && query[0] == '\0') query = NULL;          /* empty -w = all words */
    if (query != NULL) {
        if (normalize_query(query, qword, sizeof(qword)) != 0) {
            fprintf(stderr, "-w needs a word made of letters only (digits are never counted)\n");
            return 2;
        }
        qlen = strlen(qword);
    }

    filelist_t files;
    if (scan_folder(folder, &files) != 0) return 1;
    if (files.num_files == 0) {
        fprintf(stderr, "no usable .txt files in \"%s\" (%zu empty, %zu unreadable, %zu other skipped)\n",
                folder, files.empty, files.unreadable, files.ignored);
        free(files.paths);
        return 1;
    }

    mr_job_t *job1 = mr_job_create();
    mr_job_t *job2 = mr_job_create();
    char *lines = NULL;
    entry_t *entries = NULL;
    int exit_code = 1;
    if (job1 == NULL || job2 == NULL) { fprintf(stderr, "out of memory\n"); goto done; }

    /* ---- job 1 ---- */
    mr_error_t err = mr_job_set_map(job1, count_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job1, count_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job1, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job1, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job1, MR_SPLIT_LINES);   /* never cut a path */
    if (err == MR_OK) err = mr_job_set_split_size(job1, 1);                /* one path per split */
    if (err == MR_OK) err = mr_job_set_input(job1, files.paths, files.paths_len);
    if (err == MR_OK) err = mr_job_run(job1);
    if (err != MR_OK) { fprintf(stderr, "job 1 failed: %s\n", mr_error_string(err)); goto done; }

    const mr_kv_list_t *counts = mr_job_output(job1);
    unsigned long total_words = 0;
    for (size_t i = 0; i < counts->count; i++) total_words += strtoul(counts->items[i].value, NULL, 10);

    /* ---- build the input of job 2 from the output of job 1 ---- */
    size_t need = 16;
    for (size_t i = 0; i < counts->count; i++)
        need += counts->items[i].key_len + counts->items[i].value_len + 2;
    lines = malloc(need);
    entries = malloc((counts->count + 1) * sizeof(entry_t));
    if (lines == NULL || entries == NULL) { fprintf(stderr, "out of memory\n"); goto done; }
    size_t len = 0;
    for (size_t i = 0; i < counts->count; i++) {
        len += (size_t)snprintf(lines + len, need - len, "%s\t%s\n",
                                counts->items[i].key, counts->items[i].value);
        entries[i].word = counts->items[i].key;
        entries[i].count = strtoul(counts->items[i].value, NULL, 10);
    }

    /* ---- job 2 ---- */
    err = mr_job_set_map(job2, hist_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job2, count_reduce, NULL);   /* same "add up" reduce */
    if (err == MR_OK) err = mr_job_set_mappers(job2, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job2, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job2, MR_SPLIT_LINES);
    if (err == MR_OK) err = mr_job_set_input(job2, lines, len);
    if (err == MR_OK) err = mr_job_run(job2);
    if (err != MR_OK) { fprintf(stderr, "job 2 failed: %s\n", mr_error_string(err)); goto done; }

    printf("WORD FREQUENCY ANALYSIS OVER A FOLDER  (job 1: %zu mappers = one per file, %zu reducers)\n",
           mr_job_stats(job1)->mappers_used, reducers);
    print_folder_summary(folder, &files);
    printf("\n%lu words in total, %zu different words\n", total_words, counts->count);

    qsort(entries, counts->count, sizeof(entry_t), by_count_desc);
    const mr_kv_list_t *hist = mr_job_output(job2);

    if (query != NULL) {                                  /* ONE word */
        size_t rank = 0;
        unsigned long qcount = 0;
        for (size_t i = 0; i < counts->count; i++)
            if (strlen(entries[i].word) == qlen && memcmp(entries[i].word, qword, qlen) == 0) {
                rank = i + 1;
                qcount = entries[i].count;
                break;
            }
        printf("\nWord \"%s\":\n", qword);
        if (rank == 0) {
            printf("  does not occur in any file (count 0)\n");
        } else {
            unsigned long same = 0;
            for (size_t i = 0; i < hist->count; i++)
                if (strtoul(hist->items[i].key, NULL, 10) == qcount)
                    same = strtoul(hist->items[i].value, NULL, 10);
            printf("  count : %lu\n", qcount);
            printf("  share : %.4f%% of all %lu words\n",
                   100.0 * (double)qcount / (double)total_words, total_words);
            printf("  rank  : %zu of %zu different words (most frequent = 1)\n", rank, counts->count);
            printf("  %lu different word(s) occur exactly %lu time(s), including this one\n", same, qcount);
        }
    } else {                                              /* ALL words */
        printf("\nMost frequent words:\n");
        printf("  %-14s %7s %9s\n", "word", "count", "share");
        for (size_t i = 0; i < counts->count && i < 10; i++)
            printf("  %-14s %7lu %8.2f%%\n", entries[i].word, entries[i].count,
                   100.0 * (double)entries[i].count / (double)total_words);
        printf("\nFrequency of frequencies (job 2):\n");
    }

    unsigned long words_in_hist = 0, words_weighted = 0;
    for (size_t i = 0; i < hist->count; i++) {
        unsigned long c = strtoul(hist->items[i].key, NULL, 10);
        unsigned long w = strtoul(hist->items[i].value, NULL, 10);
        words_in_hist += w;
        words_weighted += c * w;
        if (query == NULL && i < 15) printf("  %lu word(s) occur %lu time(s)\n", w, c);
    }
    if (query == NULL && hist->count > 15) printf("  ... (%zu more rows)\n", hist->count - 15);

    if (out_file != NULL) {
        mr_error_t werr = mr_job_write_output(job1, out_file);
        if (werr == MR_OK) printf("\nAll %zu words written to %s\n", counts->count, out_file);
        else fprintf(stderr, "\ncannot write \"%s\": %s\n", out_file, mr_error_string(werr));
    }

    /* Verify: (1) the histogram covers every word once and adds up to all words;
     * (2) the total equals the number of words counted again by a plain loop
     * over the files, one file at a time. */
    unsigned long expected = 0;
    int readable = 1;
    char path[MAX_PATH_LEN];
    size_t pos = 0;
    while (next_path(files.paths, files.paths_len, &pos, path)) {
        size_t text_len = 0;
        char *text = read_whole_file(path, &text_len);
        if (text == NULL) { readable = 0; break; }
        expected += count_words(text, text_len);
        free(text);
    }
    int ok = readable && (words_in_hist == counts->count) && (words_weighted == total_words) &&
             (total_words == expected);
    printf("\nCheck: histogram covers %lu different words and %lu words in total; "
           "a plain loop over the files counts %lu words -> %s\n",
           words_in_hist, words_weighted, expected, ok ? "OK" : "MISMATCH");
    print_stats(job1);
    exit_code = ok ? 0 : 1;

done:
    free(lines);
    free(entries);
    mr_job_destroy(job1);
    mr_job_destroy(job2);
    free(files.paths);
    return exit_code;
}