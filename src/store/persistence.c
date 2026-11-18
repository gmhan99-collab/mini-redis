#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "persistence.h"
#include "persistence_file.h"
#include "measurement_clock.h"
#include "random_seed.h"
#include "server/http_api.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define STORE_ID_BYTES 16
#define AOF_LINE_BYTES (2u * (HTTP_MAX_KEY_BYTES + HTTP_MAX_BODY_BYTES) + 64u)
#define CLOCK_FILE "clock"
#define RDB_FILE "snapshot.rdb"
#define AOF_FILE "append.aof"

struct persistence {
    store_t *store;
    persistence_files files;
    FILE *aof;
    persistence_mode mode;
    uint8_t id[STORE_ID_BYTES];
    uint64_t generation, data_bytes, elapsed, origin, last_now;
    double restore_ms;
    int active, failed;
};

static int write_bytes(FILE *file, const void *bytes, size_t length)
{
    return fwrite(bytes, 1, length, file) == length;
}

static int read_bytes(FILE *file, void *bytes, size_t length)
{
    return fread(bytes, 1, length, file) == length;
}

static int write_u64(FILE *file, uint64_t value)
{
    unsigned char bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)(value >> (i * 8));
    }
    return write_bytes(file, bytes, sizeof(bytes));
}

static int read_u64(FILE *file, uint64_t *value)
{
    unsigned char bytes[8];
    if (!read_bytes(file, bytes, sizeof(bytes))) {
        return 0;
    }
    *value = 0;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        *value |= (uint64_t)bytes[i] << (i * 8);
    }
    return 1;
}

static int read_equal(FILE *file, const void *expected, size_t length)
{
    unsigned char bytes[STORE_ID_BYTES];
    return length <= sizeof(bytes) && read_bytes(file, bytes, length) &&
           memcmp(bytes, expected, length) == 0;
}

static int file_at_end(FILE *file)
{
    return fgetc(file) == EOF && !ferror(file);
}

typedef struct {
    const persistence *state;
    uint64_t timestamp;
    int clean;
} checkpoint;

static int write_checkpoint(FILE *file, void *context)
{
    const checkpoint *record = context;
    const persistence *state = record->state;
    return write_bytes(file, "MRC1", 4) && write_bytes(file, state->id, sizeof(state->id)) &&
           fputc(state->mode, file) != EOF && fputc(record->clean, file) != EOF &&
           write_u64(file, state->generation) && write_u64(file, record->timestamp) &&
           write_u64(file, state->data_bytes);
}

static int replace_checkpoint(const persistence *state, uint64_t timestamp, int clean,
                              const char *operation)
{
    checkpoint record = {state, timestamp, clean};
    return persistence_file_replace(&state->files, CLOCK_FILE, operation,
                                    write_checkpoint, &record, NULL) == 1;
}

static int read_checkpoint(persistence *state)
{
    FILE *file = persistence_file_open(&state->files, CLOCK_FILE, "rb");
    if (!file) {
        return 0;
    }
    int valid = read_equal(file, "MRC1", 4) && read_bytes(file, state->id, sizeof(state->id)) &&
                fgetc(file) == (int)state->mode && fgetc(file) == 1 &&
                read_u64(file, &state->generation) && read_u64(file, &state->elapsed) &&
                read_u64(file, &state->data_bytes) && file_at_end(file);
    return fclose(file) == 0 && valid;
}

typedef struct {
    const persistence *state;
    FILE *file;
    uint64_t timestamp, count;
} snapshot_writer;

static int write_snapshot_entry(const void *key, size_t key_len, const void *value,
                                size_t value_len, uint64_t expiry, void *context)
{
    snapshot_writer *writer = context;
    writer->count++;
    return write_u64(writer->file, key_len) && write_u64(writer->file, value_len) &&
           write_u64(writer->file, expiry) && write_bytes(writer->file, key, key_len) &&
           write_bytes(writer->file, value, value_len);
}

static int write_snapshot(FILE *file, void *context)
{
    snapshot_writer *writer = context;
    const persistence *state = writer->state;
    writer->file = file;
    return write_bytes(file, "MRD1", 4) && write_bytes(file, state->id, sizeof(state->id)) &&
           write_u64(file, state->generation + 1) && write_u64(file, writer->timestamp) &&
           write_u64(file, 0) &&
           store_visit_live(state->store, writer->timestamp, write_snapshot_entry, writer) &&
           fseek(file, 4 + STORE_ID_BYTES + 8 + 8, SEEK_SET) == 0 &&
           write_u64(file, writer->count);
}

static int restore_entry(persistence *state, const void *key, size_t key_len, const void *value,
                         size_t value_len, uint64_t expiry)
{
    if (expiry && expiry <= state->elapsed) {
        /* An expired replacement still supersedes any earlier AOF value. */
        store_status status = store_delete(state->store, key, key_len, state->elapsed);
        return status == STORE_OK || status == STORE_NOT_FOUND || status == STORE_EXPIRED;
    }
    return store_put(state->store, key, key_len, value, value_len,
                     expiry ? expiry - state->elapsed : 0, state->elapsed) == STORE_OK;
}

static int restore_snapshot_entry(persistence *state, FILE *file, uint64_t saved_at)
{
    uint64_t key_len, value_len, expiry;
    unsigned char key[HTTP_MAX_KEY_BYTES];
    if (!read_u64(file, &key_len) || key_len == 0 || key_len > sizeof(key) ||
        !read_u64(file, &value_len) || value_len > HTTP_MAX_BODY_BYTES ||
        !read_u64(file, &expiry) || (expiry && expiry <= saved_at) ||
        !read_bytes(file, key, (size_t)key_len)) {
        return 0;
    }
    unsigned char *value = malloc(value_len ? (size_t)value_len : 1);
    if (!value) {
        return 0;
    }
    int restored = read_bytes(file, value, (size_t)value_len) &&
                   restore_entry(state, key, (size_t)key_len, value, (size_t)value_len, expiry);
    free(value);
    return restored;
}

static int restore_snapshot(persistence *state)
{
    if (state->generation == 0) {
        return state->data_bytes == 0 && persistence_file_missing(&state->files, RDB_FILE);
    }
    FILE *file = persistence_file_open(&state->files, RDB_FILE, "rb");
    if (!file) {
        return 0;
    }
    uint64_t generation, saved_at, count, bytes;
    int valid = persistence_file_size(file, &bytes) && bytes == state->data_bytes &&
                read_equal(file, "MRD1", 4) && read_equal(file, state->id, sizeof(state->id)) &&
                read_u64(file, &generation) && generation == state->generation &&
                read_u64(file, &saved_at) && saved_at <= state->elapsed &&
                read_u64(file, &count);
    while (valid && count) {
        valid = restore_snapshot_entry(state, file, saved_at);
        count--;
    }
    valid = valid && file_at_end(file);
    return fclose(file) == 0 && valid;
}

static int write_hex(FILE *file, const void *data, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *bytes = data;
    char encoded[4096];
    while (length) {
        size_t chunk = length < sizeof(encoded) / 2 ? length : sizeof(encoded) / 2;
        for (size_t i = 0; i < chunk; i++) {
            encoded[2 * i] = hex[bytes[i] >> 4];
            encoded[2 * i + 1] = hex[bytes[i] & 15];
        }
        if (!write_bytes(file, encoded, chunk * 2)) {
            return 0;
        }
        bytes += chunk;
        length -= chunk;
    }
    return 1;
}

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    return -1;
}

static int decode_hex(char *text, size_t length)
{
    if (length % 2 != 0) {
        return 0;
    }
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_digit(text[i]);
        int low = hex_digit(text[i + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        text[i / 2] = (char)(high * 16 + low);
    }
    return 1;
}

static int read_decimal(char **cursor, uint64_t *number)
{
    char *text = *cursor;
    *number = 0;
    if (*text < '0' || *text > '9') {
        return 0;
    }
    while (*text >= '0' && *text <= '9') {
        unsigned digit = (unsigned)(*text++ - '0');
        if (*number > (UINT64_MAX - digit) / 10) {
            return 0;
        }
        *number = *number * 10 + digit;
    }
    if (*text != ' ') {
        return 0;
    }
    *cursor = text + 1;
    return 1;
}

static int replay_record(persistence *state, char *line, uint64_t *previous)
{
    char operation = line[0];
    char *key = line + 2;
    uint64_t timestamp, expiry;
    if ((operation != 'P' && operation != 'D') || line[1] != ' ' ||
        !read_decimal(&key, &timestamp) || timestamp < *previous || timestamp > state->elapsed ||
        !read_decimal(&key, &expiry) || (expiry && expiry <= timestamp)) {
        return 0;
    }
    *previous = timestamp;
    char *value = strchr(key, ' ');
    if (!value) {
        return 0;
    }
    size_t key_len = (size_t)(value++ - key);
    size_t value_len = strlen(value);
    if (!key_len || key_len > HTTP_MAX_KEY_BYTES * 2 || value_len > HTTP_MAX_BODY_BYTES * 2 ||
        !decode_hex(key, key_len) || !decode_hex(value, value_len)) {
        return 0;
    }
    if (operation == 'D') {
        if (expiry || value_len) {
            return 0;
        }
        store_status status = store_delete(state->store, key, key_len / 2, state->elapsed);
        return status == STORE_OK || status == STORE_NOT_FOUND || status == STORE_EXPIRED;
    }
    return restore_entry(state, key, key_len / 2, value, value_len / 2, expiry);
}

static int restore_aof(persistence *state)
{
    uint64_t bytes, previous = 0;
    char id[STORE_ID_BYTES * 2];
    FILE *file = state->aof;
    if (state->generation != 1 || !persistence_file_size(file, &bytes) || bytes != state->data_bytes ||
        !read_equal(file, "MRA1 ", 5) || !read_bytes(file, id, sizeof(id)) ||
        fgetc(file) != '\n' || !decode_hex(id, sizeof(id)) || memcmp(id, state->id, sizeof(state->id))) {
        return 0;
    }
    char *line = malloc(AOF_LINE_BYTES);
    if (!line) {
        return 0;
    }
    int valid = 1;
    while (valid && fgets(line, AOF_LINE_BYTES, file)) {
        char *end = strchr(line, '\n');
        if (!end) {
            valid = 0;
        } else {
            *end = '\0';
            valid = replay_record(state, line, &previous);
        }
    }
    free(line);
    /* An intervening seek is required before writing to an update stream. */
    return valid && !ferror(file) && persistence_file_seek_end(file);
}

static int create_files(persistence *state)
{
    if (!random_seed_fill(state->id, sizeof(state->id))) {
        return 0;
    }
    if (state->mode == PERSISTENCE_AOF) {
        state->generation = 1;
        state->aof = persistence_file_open(&state->files, AOF_FILE, NULL);
        if (!state->aof || !write_bytes(state->aof, "MRA1 ", 5) ||
            !write_hex(state->aof, state->id, sizeof(state->id)) || fputc('\n', state->aof) == EOF ||
            !persistence_file_sync(state->aof, "create") ||
            !persistence_file_size(state->aof, &state->data_bytes)) {
            return 0;
        }
    }
    return replace_checkpoint(state, 0, 1, "create");
}

static int open_files(persistence *state)
{
    if (!read_checkpoint(state)) {
        return 0;
    }
    if (state->mode == PERSISTENCE_RDB) {
        return restore_snapshot(state);
    }
    state->aof = persistence_file_open(&state->files, AOF_FILE, "r+b");
    return state->aof && restore_aof(state);
}

persistence *persistence_load(const persistence_options *options, const hash_table_config *config)
{
    uint64_t started = measurement_ticks();
    persistence *state = calloc(1, sizeof(*state));
    if (!state) {
        return NULL;
    }
    state->mode = options->mode;
    state->files.directory = options->directory;
    if (store_create_config(config, &state->store) != STORE_OK ||
        (state->mode != PERSISTENCE_NONE &&
         ((options->create && !persistence_files_create(options->directory)) ||
          !persistence_files_lock(&state->files, options->create) ||
          !(options->create ? create_files(state) : open_files(state))))) {
        persistence_destroy(state);
        return NULL;
    }
    state->restore_ms = measurement_ms(started, measurement_ticks());
    return state;
}

void persistence_destroy(persistence *state)
{
    if (state) {
        if (state->aof) {
            fclose(state->aof);
        }
        if (state->files.lock) {
            fclose(state->files.lock);
        }
        store_destroy(state->store);
        free(state);
    }
}

store_t *persistence_store(persistence *state)
{
    return state->store;
}

persistence_mode persistence_current_mode(const persistence *state)
{
    return state->mode;
}

int persistence_failed(const persistence *state)
{
    return state->failed;
}

static uint64_t clock_sample(uint64_t monotonic_ms)
{
#ifdef PERSISTENCE_TEST_HOOKS
    const char *clock_path = getenv("MINI_REDIS_TEST_CLOCK_PATH");
    FILE *clock = clock_path ? fopen(clock_path, "rb") : NULL;
    if (clock) {
        uint64_t controlled;
        if (fscanf(clock, "%" SCNu64, &controlled) == 1) {
            monotonic_ms = controlled;
        }
        fclose(clock);
    }
#endif
    return monotonic_ms;
}

int persistence_begin(persistence *state, uint64_t (*monotonic_ms)(void))
{
    if (state->mode != PERSISTENCE_NONE && !replace_checkpoint(state, state->elapsed, 0, "begin")) {
        return 0;
    }
    state->origin = clock_sample(monotonic_ms());
    state->last_now = state->elapsed;
    state->active = 1;
    return 1;
}

uint64_t persistence_now(persistence *state, uint64_t monotonic_ms)
{
    const uint64_t sampled = clock_sample(monotonic_ms);
    const uint64_t elapsed = sampled >= state->origin ? sampled - state->origin : 0;
    const uint64_t now = elapsed > UINT64_MAX - state->elapsed ? UINT64_MAX : state->elapsed + elapsed;
    if (now > state->last_now) {
        state->last_now = now;
    }
    return state->last_now;
}

int persistence_finish(persistence *state, uint64_t monotonic_ms, double *elapsed_ms)
{
    *elapsed_ms = 0;
    if (state->failed || !state->active) {
        return !state->failed;
    }
    uint64_t now = persistence_now(state, monotonic_ms);
    uint64_t started = measurement_ticks();
    int closed = 1;
    if (state->aof) {
        closed = fclose(state->aof) == 0;
        state->aof = NULL;
    }
    if (!closed || (state->mode != PERSISTENCE_NONE && !replace_checkpoint(state, now, 1, "checkpoint"))) {
        state->failed = 1;
        return 0;
    }
    *elapsed_ms = measurement_ms(started, measurement_ticks());
    state->active = 0;
    return 1;
}

int persistence_save(persistence *state, uint64_t now_ms, double *elapsed_ms)
{
    if (state->mode != PERSISTENCE_RDB || !state->active || state->failed ||
        state->generation == UINT64_MAX) {
        return 0;
    }
    uint64_t started = measurement_ticks();
    snapshot_writer writer = {state, NULL, now_ms, 0};
    int result = persistence_file_replace(&state->files, RDB_FILE, "snapshot",
                                          write_snapshot, &writer, &state->data_bytes);
    if (result != 1) {
        state->failed = result < 0;
        return 0;
    }
    state->generation++;
    *elapsed_ms = measurement_ms(started, measurement_ticks());
    return 1;
}

int persistence_status_json(const persistence *state, uint64_t now_ms, char *out, size_t capacity)
{
    static const char *const modes[] = {"none", "rdb", "aof"};
    static const char *const policies[] = {"none", "manual_snapshot", "per_mutation"};
    int length = snprintf(out, capacity, "{\"mode\":\"%s\",\"sync_policy\":\"%s\","
                         "\"format_version\":%s,\"data_bytes\":%" PRIu64 ",\"restore_ms\":%.9f,"
                         "\"logical_elapsed_ms\":%" PRIu64 "}",
                         modes[state->mode], policies[state->mode],
                         state->mode == PERSISTENCE_NONE ? "null" : "1",
                         state->data_bytes, state->restore_ms, now_ms);
    return length >= 0 && (size_t)length < capacity;
}

static store_status append_mutation(persistence *state, char operation, const void *key,
                                   size_t key_len, const void *value, size_t value_len,
                                   uint64_t expiry, uint64_t now_ms)
{
    FILE *file = state->aof;
    if (fprintf(file, "%c %" PRIu64 " %" PRIu64 " ", operation, now_ms, expiry) < 0 ||
        !write_hex(file, key, key_len) || fputc(' ', file) == EOF ||
        !write_hex(file, value, value_len) || fputc('\n', file) == EOF ||
        !persistence_file_sync(file, "append") || !persistence_file_size(file, &state->data_bytes)) {
        state->failed = 1;
        return STORE_PERSISTENCE_ERROR;
    }
    return STORE_OK;
}

store_status persistence_put(persistence *state, const void *key, size_t key_len,
                             const void *value, size_t value_len, uint64_t ttl_ms, uint64_t now_ms)
{
    if (state->failed || !state->active) {
        return STORE_PERSISTENCE_ERROR;
    }
    if (!key_len || key_len > HTTP_MAX_KEY_BYTES || value_len > HTTP_MAX_BODY_BYTES) {
        return STORE_INVALID_ARGUMENT;
    }
    store_status status = store_put(state->store, key, key_len, value, value_len, ttl_ms, now_ms);
    if (status == STORE_OK && state->mode == PERSISTENCE_AOF) {
        return append_mutation(state, 'P', key, key_len, value, value_len,
                               ttl_ms ? now_ms + ttl_ms : 0, now_ms);
    }
    return status;
}

store_status persistence_delete(persistence *state, const void *key, size_t key_len, uint64_t now_ms)
{
    if (state->failed || !state->active) {
        return STORE_PERSISTENCE_ERROR;
    }
    if (!key_len || key_len > HTTP_MAX_KEY_BYTES) {
        return STORE_INVALID_ARGUMENT;
    }
    store_status status = store_delete(state->store, key, key_len, now_ms);
    if (status == STORE_OK && state->mode == PERSISTENCE_AOF) {
        return append_mutation(state, 'D', key, key_len, NULL, 0, 0, now_ms);
    }
    return status;
}
