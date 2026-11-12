/*
 * max_per_key_folder.c
 * Maximum DATA_VALUE for each (station ID, weather ELEMENT)
 * across every .csv file in a folder.
 *
 * Supports plain CSV and gzip-compressed files with a .csv extension.
 * Expected GHCN-Daily field order:
 * ID,DATE,ELEMENT,DATA_VALUE,M_FLAG,Q_FLAG,S_FLAG,OBS_TIME
 *
 * Key   = ID|ELEMENT
 * Value = DATA_VALUE (kept in the dataset's original units/scaling)
 *
 * Build with the MapReduce framework sources, pthread and zlib:
 *   gcc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -Iinclude -Isrc -Iexamples examples/max_per_key_folder.c \
 *       src/*.c -pthread -lz -lm -o examples/max_per_key_folder
 *
 * Run:
 *   ./examples/max_per_key_folder /path/to/Max_val_per_key
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <unistd.h>
#include <zlib.h>
#include <float.h>

#include "app_util.h"

#define CHUNK_SIZE       (32u * 1024u * 1024u)  /* 32 MB decompressed */
#define SPLIT_SIZE       (4u * 1024u * 1024u)   /* 4 MB per MapReduce split */
#define KEY_BUFFER_SIZE  128

typedef struct {
    char *key;
    double max_value;
    uint64_t hash;
} result_row_t;

typedef struct {
    result_row_t *slots;
    size_t capacity; /* always a power of two */
    size_t count;
} result_table_t;

/* FNV-1a hash. */
static uint64_t hash_key(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* Resize the hash table and reinsert existing entries. */
static int resize_table(result_table_t *table, size_t new_capacity)
{
    result_row_t *new_slots =
        calloc(new_capacity, sizeof(result_row_t));
    if (new_slots == NULL) return 0;

    for (size_t i = 0; i < table->capacity; i++) {
        if (table->slots[i].key == NULL) continue;

        size_t pos = (size_t)table->slots[i].hash & (new_capacity - 1);
        while (new_slots[pos].key != NULL)
            pos = (pos + 1) & (new_capacity - 1);

        new_slots[pos] = table->slots[i];
    }

    free(table->slots);
    table->slots = new_slots;
    table->capacity = new_capacity;
    return 1;
}

/* Insert a key or update its maximum. Average lookup is O(1). */
static int update_global_table(result_table_t *table,
                               const char *key, double value)
{
    if (table->capacity == 0) {
        if (!resize_table(table, 1024)) return 0;
    }

    if ((table->count + 1) * 10 >= table->capacity * 7) {
        if (!resize_table(table, table->capacity * 2)) return 0;
    }

    uint64_t hash = hash_key(key);
    size_t pos = (size_t)hash & (table->capacity - 1);

    while (table->slots[pos].key != NULL) {
        if (table->slots[pos].hash == hash &&
            strcmp(table->slots[pos].key, key) == 0) {
            if (value > table->slots[pos].max_value)
                table->slots[pos].max_value = value;
            return 1;
        }
        pos = (pos + 1) & (table->capacity - 1);
    }

    char *copy = malloc(strlen(key) + 1);
    if (copy == NULL) return 0;
    strcpy(copy, key);

    table->slots[pos].key = copy;
    table->slots[pos].max_value = value;
    table->slots[pos].hash = hash;
    table->count++;
    return 1;
}

/*
 * Read the fields needed from one GHCN-Daily CSV row.
 * The first six fields are enough; OBS_TIME is not used.
 * Rows with a non-empty Q_FLAG or missing-value sentinel -9999 are skipped.
 */
static int parse_weather_row(const char *line, size_t len,
                             char *key, size_t key_capacity, double *value)
{
    const char *field[6];
    size_t field_len[6];
    size_t start = 0, field_count = 0;

    for (size_t i = 0; i <= len && field_count < 6; i++) {
        if (i == len || line[i] == ',') {
            field[field_count] = line + start;
            field_len[field_count] = i - start;
            field_count++;
            start = i + 1;
        }
    }

    if (field_count < 6) return 0;
    if (field_len[0] == 0 || field_len[2] == 0 || field_len[3] == 0)
        return 0;

    /* Ignore observations marked as failing the quality check. */
    if (field_len[5] != 0) return 0;

    char number[64];
    if (field_len[3] >= sizeof(number)) return 0;
    memcpy(number, field[3], field_len[3]);
    number[field_len[3]] = '\0';

    char *end = NULL;
    double v = strtod(number, &end);
    if (end == number || *end != '\0' || v == -9999.0)
        return 0;

    size_t needed = field_len[0] + 1 + field_len[2] + 1;
    if (needed > key_capacity) return 0;

    memcpy(key, field[0], field_len[0]);
    key[field_len[0]] = '|';
    memcpy(key + field_len[0] + 1, field[2], field_len[2]);
    key[field_len[0] + 1 + field_len[2]] = '\0';

    *value = v;
    return 1;
}

/* Map: emit (station|element, DATA_VALUE). */
static void weather_map(const char *input, size_t len,
                        mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    const char *line;
    size_t line_len, pos = 0;

    while (next_line(input, len, &pos, &line, &line_len)) {
        char key[KEY_BUFFER_SIZE];
        double value;

        if (!parse_weather_row(line, line_len, key, sizeof(key), &value))
            continue;

        char number[64];
        int n = snprintf(number, sizeof(number), "%.10g", value);
        if (n > 0)
            mr_emit(out, key, strlen(key), number, (size_t)n);
    }
}

/* Reduce: find the maximum DATA_VALUE for one station|element key. */
static void weather_reduce(const char *key, size_t key_len,
                           const mr_kv_t *values, size_t num_values,
                           mr_emitter_t *out, void *ctx)
{
    (void)ctx;
    double best = -DBL_MAX;

    for (size_t i = 0; i < num_values; i++) {
        double v = strtod(values[i].value, NULL);
        if (v > best) best = v;
    }

    char number[64];
    int n = snprintf(number, sizeof(number), "%.10g", best);
    if (n > 0)
        mr_emit(out, key, key_len, number, (size_t)n);
}

/* Run one in-memory chunk through the MapReduce framework. */
static int run_chunk(const char *data, size_t length,
                     result_table_t *global, size_t file_number,
                     size_t chunk_number)
{
    printf("  File %zu | chunk %zu: running MapReduce...\n",
           file_number, chunk_number);
    fflush(stdout);

    mr_job_t *job = mr_job_create();
    if (job == NULL) {
        fprintf(stderr, "Could not create MapReduce job (out of memory).\n");
        return 0;
    }

    mr_error_t err = mr_job_set_map(job, weather_map, NULL);
    if (err == MR_OK) err = mr_job_set_reduce(job, weather_reduce, NULL);
    if (err == MR_OK) err = mr_job_set_mappers(job, 0);
    if (err == MR_OK) err = mr_job_set_reducers(job, 2);
    if (err == MR_OK) err = mr_job_set_split_mode(job, MR_SPLIT_LINES);
    if (err == MR_OK) err = mr_job_set_split_size(job, SPLIT_SIZE);
    if (err == MR_OK) err = mr_job_set_input(job, data, length);
    if (err == MR_OK) err = mr_job_run(job);

    if (err != MR_OK) {
        fprintf(stderr, "MapReduce error: %s\n", mr_error_string(err));
        mr_job_destroy(job);
        return 0;
    }

    const mr_kv_list_t *output = mr_job_output(job);
    for (size_t i = 0; i < output->count; i++) {
        double value = strtod(output->items[i].value, NULL);
        if (!update_global_table(global, output->items[i].key, value)) {
            fprintf(stderr, "Could not merge MapReduce output (out of memory).\n");
            mr_job_destroy(job);
            return 0;
        }
    }

    printf("  File %zu | chunk %zu: complete (%zu keys)\n",
           file_number, chunk_number, output->count);
    fflush(stdout);

    mr_job_destroy(job);
    return 1;
}

/* Stream a file through bounded 32 MB decompressed chunks. gzopen transparently
   reads both gzip-compressed and ordinary files. */
static int process_one_file(const char *path, result_table_t *global,
                            size_t file_number)
{
    gzFile input = gzopen(path, "rb");
    if (input == NULL) {
        fprintf(stderr, "Could not open: %s\n", path);
        return 0;
    }

    char *buffer = malloc(CHUNK_SIZE + 1024u * 1024u);
    if (buffer == NULL) {
        gzclose(input);
        fprintf(stderr, "Could not allocate chunk buffer.\n");
        return 0;
    }

    printf("\nFile %zu: %s\n", file_number, path);
    fflush(stdout);

    size_t used = 0;
    size_t chunk_number = 0;
    int good = 1;
    int bytes_read = 0;

    while ((bytes_read = gzread(input, buffer + used,
                                (unsigned int)(CHUNK_SIZE - used))) > 0) {
        used += (size_t)bytes_read;

        if (used < CHUNK_SIZE)
            continue;

        /* Cut only at a newline so a CSV record is never split. */
        size_t cut = used;
        while (cut > 0 && buffer[cut - 1] != '\n')
            cut--;

        if (cut == 0) {
            fprintf(stderr, "No newline found within chunk in %s\n", path);
            good = 0;
            break;
        }

        chunk_number++;
        if (!run_chunk(buffer, cut, global, file_number, chunk_number)) {
            good = 0;
            break;
        }

        memmove(buffer, buffer + cut, used - cut);
        used -= cut;
    }

    if (bytes_read < 0) {
        int error_number;
        const char *message = gzerror(input, &error_number);
        fprintf(stderr, "Read error in %s: %s\n", path, message);
        good = 0;
    }

    if (good && used > 0) {
        chunk_number++;
        if (!run_chunk(buffer, used, global, file_number, chunk_number))
            good = 0;
    }

    if (gzclose(input) != Z_OK)
        good = 0;

    free(buffer);

    if (good) {
        printf("Completed file %zu (%zu chunks).\n",
               file_number, chunk_number);
        fflush(stdout);
    }

    return good;
}

static int has_csv_extension(const char *name)
{
    size_t n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, ".csv") == 0;
}

static int compare_names(const void *a, const void *b)
{
    const char * const *x = (const char * const *)a;
    const char * const *y = (const char * const *)b;
    return strcmp(*x, *y);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <folder-containing-csv-files>\n", argv[0]);
        return 2;
    }

    DIR *dir = opendir(argv[1]);
    if (dir == NULL) {
        perror("Could not open dataset folder");
        return 1;
    }

    char **files = NULL;
    size_t file_count = 0, file_capacity = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (!has_csv_extension(entry->d_name)) continue;

        if (file_count == file_capacity) {
            size_t new_capacity = file_capacity == 0 ? 16 : file_capacity * 2;
            char **bigger = realloc(files, new_capacity * sizeof(char *));
            if (bigger == NULL) {
                closedir(dir);
                return 1;
            }
            files = bigger;
            file_capacity = new_capacity;
        }

        files[file_count] = malloc(strlen(entry->d_name) + 1);
        if (files[file_count] == NULL) {
            closedir(dir);
            return 1;
        }
        strcpy(files[file_count], entry->d_name);
        file_count++;
    }
    closedir(dir);

    if (file_count == 0) {
        fprintf(stderr, "No .csv files found in %s\n", argv[1]);
        free(files);
        return 1;
    }

    qsort(files, file_count, sizeof(char *), compare_names);

    printf("Found %zu CSV files.\n", file_count);
    printf("Chunk size: %u MB | MapReduce split size: %u MB\n",
           (unsigned)(CHUNK_SIZE / (1024u * 1024u)),
           (unsigned)(SPLIT_SIZE / (1024u * 1024u)));
    fflush(stdout);

    result_table_t global = {0};
    size_t processed = 0;
    char path[4096];

    for (size_t i = 0; i < file_count; i++) {
        int n = snprintf(path, sizeof(path), "%s/%s", argv[1], files[i]);

        if (n < 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "Path too long: %s\n", files[i]);
            free(files[i]);
            continue;
        }

        if (process_one_file(path, &global, i + 1))
            processed++;

        free(files[i]);
    }
    free(files);

    if (processed == 0) {
        fprintf(stderr, "No files were processed successfully.\n");
        for (size_t i = 0; i < global.capacity; i++)
            free(global.slots[i].key);
        free(global.slots);
        return 1;
    }

    printf("\n========== MAXIMUM VALUE PER KEY ==========\n");
    printf("Successfully processed %zu of %zu files.\n\n",
           processed, file_count);
    printf("%-16s %-8s %s\n", "STATION_ID", "ELEMENT", "MAX_DATA_VALUE");

    for (size_t i = 0; i < global.capacity; i++) {
        if (global.slots[i].key == NULL) continue;

        char *separator = strchr(global.slots[i].key, '|');
        if (separator != NULL) {
            *separator = '\0';
            printf("%-16s %-8s %.10g\n",
                   global.slots[i].key, separator + 1,
                   global.slots[i].max_value);
            *separator = '|';
        }
    }

    printf("\nTotal unique station-element keys: %zu\n", global.count);

    for (size_t i = 0; i < global.capacity; i++)
        free(global.slots[i].key);
    free(global.slots);

    return processed == file_count ? 0 : 1;
}
