#define _POSIX_C_SOURCE 200809L        
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h>

#include "app_util.h"

#define MAX_PATH_LEN 4096

typedef struct {
    char  *paths;                       
    size_t paths_len;
    size_t num_files;                    
    size_t empty, unreadable, ignored;   
} filelist_t;

static int has_txt_extension(const char *name)
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
        qsort(list, count, sizeof(char *), cmp_path);
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

typedef struct { char *word; size_t len; unsigned long count; } slot_t;
typedef struct { slot_t *slots; size_t size, used; } wordtab_t;

static size_t hash_word(const char *w, size_t len)
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
    if ((t->used + 1) * 2 > t->size) {
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

static int count_file(const char *path, wordtab_t *t)
{
    size_t len = 0;
    char *text = read_whole_file(path, &len);
    if (text == NULL) return -1;
    char word[128];
    size_t word_len, pos = 0;
    int rc = 0;
    while (rc == 0 && next_word(text, len, &pos, word, sizeof(word), &word_len)) {
        if (word_len < 3) continue;

        /* STRICT ALPHA FILTER: Every character MUST be an alphabet letter */
        int only_alpha = 1;
        for (size_t i = 0; i < word_len; i++) {
            if (!isalpha((unsigned char)word[i])) {
                only_alpha = 0;
                break;
            }
        }
        if (!only_alpha) continue;

        rc = tab_add(t, word, word_len);
    }
    free(text);
    return rc;
}

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

__attribute__((unused)) static unsigned long count_words(const char *text, size_t len)
{
    char word[128];
    size_t word_len, pos = 0;
    unsigned long n = 0;
    while (next_word(text, len, &pos, word, sizeof(word), &word_len)) n++;
    return n;
}

static const char *doc_id_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void iif_map(const char *input, size_t len, mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    char path[MAX_PATH_LEN];
    size_t pos = 0;
    while (next_path(input, len, &pos, path)) {
        const char *doc = doc_id_of(path);
        size_t doc_len = strlen(doc);
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
            mr_emit(out, tab.slots[i].word, tab.slots[i].len, doc, doc_len);
        }
        tab_free(&tab);
    }
}

static void iif_reduce(const char *key, size_t key_len,
                       const mr_kv_t *values, size_t num_values,
                       mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    size_t total = 0;
    for (size_t i = 0; i < num_values; i++) total += values[i].value_len + 1;
    char *list = malloc(total + 1);
    if (list == NULL) { mr_emit_fail(out); return; }

    size_t n = 0;
    for (size_t i = 0; i < num_values; i++) {
        if (i > 0 && values[i].value_len == values[i - 1].value_len &&
            memcmp(values[i].value, values[i - 1].value, values[i].value_len) == 0)
            continue;
        if (n > 0) list[n++] = ',';
        memcpy(list + n, values[i].value, values[i].value_len);
        n += values[i].value_len;
    }
    mr_emit(out, key, key_len, list, n);
    free(list);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static long distinct_words_in_file(const char *path)
{
    size_t text_len = 0;
    char *text = read_whole_file(path, &text_len);
    if (text == NULL) return -1;
    size_t cap = 1024, n = 0;
    char **words = malloc(cap * sizeof(char *));
    if (words == NULL) { free(text); return -1; }
    char word[128];
    size_t word_len, pos = 0;
    long distinct = -1;
    int failed = 0;
    while (!failed && next_word(text, text_len, &pos, word, sizeof(word), &word_len)) {
        if (word_len < 3) continue;

        int only_alpha = 1;
        for (size_t i = 0; i < word_len; i++) {
            if (!isalpha((unsigned char)word[i])) {
                only_alpha = 0;
                break;
            }
        }
        if (!only_alpha) continue;

        if (n == cap) {
            char **bigger = realloc(words, cap * 2 * sizeof(char *));
            if (bigger == NULL) { failed = 1; break; }
            words = bigger;
            cap *= 2;
        }
        words[n] = malloc(word_len + 1);
        if (words[n] == NULL) { failed = 1; break; }
        memcpy(words[n], word, word_len + 1);
        n++;
    }
    if (!failed) {
        qsort(words, n, sizeof(char *), cmp_str);
        distinct = 0;
        for (size_t i = 0; i < n; i++)
            if (i == 0 || strcmp(words[i], words[i - 1]) != 0) distinct++;
    }
    for (size_t i = 0; i < n; i++) free(words[i]);
    free(words);
    free(text);
    return distinct;
}

static int is_known_doc(char *const *names, size_t num_names, const char *id, size_t id_len)
{
    size_t lo = 0, hi = num_names;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        size_t name_len = strlen(names[mid]);
        size_t common = name_len < id_len ? name_len : id_len;
        int c = memcmp(names[mid], id, common);
        if (c == 0) c = (name_len < id_len) ? -1 : (name_len > id_len ? 1 : 0);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <folder> [max-mappers] [reducers] [-o output-file] [query words...]\n", argv[0]);
        return 2;
    }
    const char *folder = argv[1];
    size_t mappers = 0, reducers = 2;
    const char *out_file = NULL;
    int first_query = argc; /* Default: no query words */

    /* Smart CLI parsing: check if argv[2] is numeric mappers or a search term */
    if (argc > 2) {
        if (parse_positive(argv[2], &mappers)) {
            /* argv[2] is a valid mapper count */
            if (argc > 3 && parse_positive(argv[3], &reducers)) {
                /* argv[3] is a valid reducer count */
                first_query = 4;
            } else {
                first_query = 3;
            }
        } else {
            /* argv[2] is not a number, so it must be a search term directly */
            first_query = 2;
        }
    }

    /* Check for -o flag inside arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                out_file = argv[i + 1];
            } else {
                fprintf(stderr, "-o needs a file name\n");
                return 2;
            }
        }
    }

    filelist_t files;
    if (scan_folder(folder, &files) != 0) return 1;
    if (files.num_files == 0) {
        fprintf(stderr, "no usable .txt files in \"%s\" (%zu empty, %zu unreadable, %zu other skipped)\n",
                folder, files.empty, files.unreadable, files.ignored);
        free(files.paths);
        return 1;
    }

    mr_job_t *job = mr_job_create();
    if (job == NULL) { fprintf(stderr, "out of memory\n"); free(files.paths); return 1; }

    mr_error_t err = mr_job_set_map(job, iif_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job, iif_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job, mappers);
    if (err == MR_OK) err = mr_job_set_reducers(job, reducers);
    if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_LINES);
    if (err == MR_OK) err = mr_job_set_split_size(job, 1);
    if (err == MR_OK) err = mr_job_set_input(job, files.paths, files.paths_len);
    if (err == MR_OK) err = mr_job_run(job);
    if (err != MR_OK) {
        fprintf(stderr, "inverted_index_folder failed: %s\n", mr_error_string(err));
        mr_job_destroy(job);
        free(files.paths);
        return 1;
    }

    const mr_kv_list_t *out = mr_job_output(job);
    printf("INVERTED INDEX OVER A FOLDER  (%zu mappers = one per document, %zu reducers)\n",
           mr_job_stats(job)->mappers_used, reducers);
    print_folder_summary(folder, &files);
    printf("\n");

    /* WRITE COMPLETE INDEX TO TXT FILE */
    const char *target_file = out_file ? out_file : "output_index.txt";
    mr_error_t werr = mr_job_write_output(job, target_file);
    if (werr == MR_OK) {
        printf(">>> COMPLETE INDEX (%zu words) successfully saved to '%s' <<<\n\n", out->count, target_file);
    } else {
        fprintf(stderr, "\ncannot write \"%s\": %s\n", target_file, mr_error_string(werr));
    }

    /* SEARCH FUNCTIONALITY */
    for (int q = first_query; q < argc; q++) {
        if (strcmp(argv[q], "-o") == 0) { q++; continue; }

        char search_term[128];
        size_t slen = strlen(argv[q]);
        if (slen >= sizeof(search_term)) slen = sizeof(search_term) - 1;
        for (size_t i = 0; i < slen; i++) search_term[i] = tolower((unsigned char)argv[q][i]);
        search_term[slen] = '\0';

        int found = 0;
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->items[i].key, search_term) == 0) {
                printf("SEARCH RESULT for '%s':\n%s\n\n", argv[q], out->items[i].value);
                found = 1;
                break;
            }
        }
        if (!found) printf("SEARCH RESULT for '%s': No matching documents found.\n\n", argv[q]);
    }

    char **names = calloc(files.num_files, sizeof(char *));
    int ok = (names != NULL);
    size_t num_names = 0;
    long expected = 0;
    if (ok) {
        char path[MAX_PATH_LEN];
        size_t pos = 0;
        while (ok && next_path(files.paths, files.paths_len, &pos, path)) {
            const char *doc = doc_id_of(path);
            names[num_names] = malloc(strlen(doc) + 1);
            if (names[num_names] == NULL) { ok = 0; break; }
            strcpy(names[num_names++], doc);
            long d = distinct_words_in_file(path);
            if (d < 0) ok = 0; else expected += d;
        }
    }
    long listed = 0;
    int ids_known = 1, ids_ascending = 1;
    for (size_t i = 0; ok && i < out->count; i++) {
        const mr_kv_t *p = &out->items[i];
        size_t start = 0;
        const char *prev = NULL;
        size_t prev_len = 0;
        for (size_t k = 0; k <= p->value_len; k++) {
            if (k < p->value_len && p->value[k] != ',') continue;
            const char *id = p->value + start;
            size_t id_len = k - start;
            listed++;
            if (!is_known_doc(names, num_names, id, id_len)) ids_known = 0;
            if (prev != NULL) {
                size_t m = prev_len < id_len ? prev_len : id_len;
                int c = memcmp(prev, id, m);
                if (c > 0 || (c == 0 && prev_len >= id_len)) ids_ascending = 0;
            }
            prev = id;
            prev_len = id_len;
            start = k + 1;
        }
    }
    int exit_code = (ok && ids_known && ids_ascending && listed == expected) ? 0 : 1;
    printf("Check: every document id is a real file name -> %s;  ids of a word ascending, none twice -> %s\n",
           ids_known ? "OK" : "MISMATCH", ids_ascending ? "OK" : "MISMATCH");
    printf("Check: %ld (word, document) entries in the index, %ld by a plain loop over the files -> %s\n",
           listed, expected, (ok && listed == expected) ? "OK" : "MISMATCH");
    print_stats(job);

    for (size_t i = 0; names != NULL && i < num_names; i++) free(names[i]);
    free(names);
    mr_job_destroy(job);
    free(files.paths);
    return exit_code;
}