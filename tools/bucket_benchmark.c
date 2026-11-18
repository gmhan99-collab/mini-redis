#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "hash_table.h"
#include "bucket_options.h"
#include "key_hash.h"
#include "measurement_clock.h"
#include "build_info.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t count, queries, rounds, seed, hit_percent;
    hash_table_config table;
    uint8_t hash_seed[16];
    int collision, prepare_only;
} bucket_bench_config;
typedef struct {
    char key[48], value[32];
    size_t key_length, value_length;
} bucket_record;
typedef struct {
    bucket_record *records;
    uint32_t *query_ids;
    uint64_t attempts;
    size_t modulus;
    double generation_ms;
    int initial_concentrated, final_concentrated;
} bucket_dataset;
typedef struct {
    double elapsed_ms;
    int ok, rehash;
} insertion_sample;
typedef struct {
    double insertion_ms, lookup_ms;
    uint64_t hits, misses, checksum;
    int verified;
    uint32_t attempted_insertions, insertion_successes, insertion_failures;
    int lookup_executed;
    insertion_sample *samples;
    char *description;
} bucket_round;

static uint32_t dataset_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int read_options(int argc, char **argv, bucket_bench_config *config)
{
    *config = (bucket_bench_config){.count = 100, .queries = 1000, .rounds = 3,
        .seed = 1, .hit_percent = 80, .table = {16, HASH_TABLE_LIST, NULL}};
    for (size_t i = 0; i < 16; i++) {
        config->hash_seed[i] = (uint8_t)i;
    }
    unsigned seen = 0;
    static const char *const names[] = {"--count", "--queries", "--rounds", "--seed", "--hit-percent",
        "--bucket-structure", "--initial-buckets", "--hash-seed", "--distribution", "--prepare-only"};
    for (int i = 1; i < argc; i++) {
        unsigned option = 10;
        for (unsigned j = 0; j < 10; j++) {
            if (!strcmp(argv[i], names[j])) {
                option = j;
            }
        }
        if (option == 10 || (seen & (1u << option))) {
            return 0;
        }
        seen |= 1u << option;
        if (option == 9) {
            config->prepare_only = 1;
            continue;
        }
        if (++i == argc) {
            return 0;
        }
        uint64_t number;
        if (option < 5) {
            uint32_t *fields[] = {&config->count, &config->queries, &config->rounds, &config->seed, &config->hit_percent};
            const uint64_t maxima[] = {10000, 1000000, 20, UINT32_MAX, 100};
            if (!bucket_number(argv[i], option < 3 ? 1 : 0, maxima[option], &number)) {
                return 0;
            }
            *fields[option] = (uint32_t)number;
        } else if (option == 5) {
            if (!bucket_structure(argv[i], &config->table.bucket_structure)) {
                return 0;
            }
        } else if (option == 6) {
            if (!bucket_number(argv[i], 1, 65536, &number) || (number & (number - 1))) {
                return 0;
            }
            config->table.bucket_count = (size_t)number;
        } else if (option == 7) {
            if (!bucket_seed(argv[i], config->hash_seed)) {
                return 0;
            }
        } else {
            if (strcmp(argv[i], "normal") && strcmp(argv[i], "collision")) {
                return 0;
            }
            config->collision = !strcmp(argv[i], "collision");
        }
    }
    config->table.seed = config->hash_seed;
    return 1;
}

static void free_dataset(bucket_dataset *data)
{
    free(data->records);
    free(data->query_ids);
}

static int prepare_dataset(const bucket_bench_config *config, bucket_dataset *data)
{
    memset(data, 0, sizeof(*data));
    data->records = calloc((size_t)config->count * 2, sizeof(*data->records));
    data->query_ids = malloc((size_t)config->queries * sizeof(*data->query_ids));
    if (!data->records || !data->query_ids) {
        return 0;
    }
    data->modulus = config->table.bucket_count;
    while (data->modulus < config->count) {
        data->modulus *= 2;
    }
    const uint64_t start = measurement_ticks();
    uint32_t generated = 0;
    while (generated < config->count * 2) {
        if (data->attempts >= UINT64_C(500000000) ||
            (!(data->attempts % 1024) && measurement_ms(start, measurement_ticks()) >= 10000.0)) {
            fprintf(stderr, "Dataset generation limit exceeded after %" PRIu64 " candidates\n", data->attempts);
            return 0;
        }
        bucket_record *record = &data->records[generated];
        record->key_length = (size_t)snprintf(record->key, sizeof(record->key), "b%08" PRIx32 "-%" PRIu64,
                                              config->seed, data->attempts++);
        const uint64_t hash = key_hash_siphash24(config->hash_seed, record->key, record->key_length);
        if (config->collision && hash % data->modulus != 0) {
            continue;
        }
        record->value_length = (size_t)snprintf(record->value, sizeof(record->value), "v%" PRIu32, generated);
        generated++;
    }
    data->initial_concentrated = 1;
    data->final_concentrated = 1;
    for (uint32_t i = 0; i < config->count * 2; i++) {
        const bucket_record *record = &data->records[i];
        const uint64_t hash = key_hash_siphash24(config->hash_seed, record->key, record->key_length);
        if (hash % config->table.bucket_count) {
            data->initial_concentrated = 0;
        }
        if (hash % data->modulus) {
            data->final_concentrated = 0;
        }
    }
    const uint32_t hit_count = (uint32_t)((uint64_t)config->queries * config->hit_percent / 100);
    uint32_t state = config->seed;
    for (uint32_t i = 0; i < config->queries; i++) {
        data->query_ids[i] = dataset_random(&state) % config->count + (i < hit_count ? 0 : config->count);
    }
    for (uint32_t i = config->queries; i > 1; i--) {
        const uint32_t other = dataset_random(&state) % i;
        const uint32_t temporary = data->query_ids[i - 1];
        data->query_ids[i - 1] = data->query_ids[other];
        data->query_ids[other] = temporary;
    }
    data->generation_ms = measurement_ms(start, measurement_ticks());
    return 1;
}

static void print_config(const bucket_bench_config *config)
{
    printf("\"config\":{\"count\":%" PRIu32 ",\"queries\":%" PRIu32 ",\"rounds\":%" PRIu32
           ",\"seed\":%" PRIu32 ",\"hit_percent\":%" PRIu32 ",\"bucket_structure\":\"%s\","
           "\"initial_buckets\":%zu,\"hash_seed\":\"", config->count, config->queries, config->rounds,
           config->seed, config->hit_percent, hash_table_structure_name(config->table.bucket_structure), config->table.bucket_count);
    for (size_t i = 0; i < 16; i++) {
        printf("%02x", config->hash_seed[i]);
    }
    printf("\",\"distribution\":\"%s\"}", config->collision ? "collision" : "normal");
}

static int compare_clock_samples(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void print_clock_overhead(void)
{
    double samples[256];
    for (size_t i = 0; i < 256; i++) {
        const uint64_t start = measurement_ticks();
        const uint64_t end = measurement_ticks();
        samples[i] = measurement_ms(start, end);
    }
    qsort(samples, 256, sizeof(*samples), compare_clock_samples);
    printf(",\"clock_pair_delta_ms\":{\"samples\":256,\"min\":%.9f,\"median\":%.9f,\"max\":%.9f}",
           samples[0], (samples[127] + samples[128]) / 2, samples[255]);
}

static void print_clock_properties(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        printf(",\"clock_frequency_hz\":%" PRIu64 ",\"clock_resolution_ms\":%.12g",
               (uint64_t)frequency.QuadPart, 1000.0 / (double)frequency.QuadPart);
    } else {
        printf(",\"clock_frequency_hz\":null,\"clock_resolution_ms\":null");
    }
#else
    /* measurement_ticks encodes CLOCK_MONOTONIC in nanosecond units. This is
     * a tick-unit frequency, not a hardware frequency or a resolution claim. */
    printf(",\"clock_frequency_hz\":1000000000,\"clock_resolution_ms\":");
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution) == 0) {
        printf("%.12g", (double)resolution.tv_sec * 1000.0 + (double)resolution.tv_nsec / 1000000.0);
    } else {
        printf("null");
    }
#endif
}

static void print_metadata(const bucket_dataset *data)
{
    printf("\"metadata\":{\"generation_attempts\":%" PRIu64 ",\"collision_modulus\":%zu,"
           "\"collision_target\":0,\"initial_concentrated\":%s,\"final_concentrated\":%s,"
           "\"generation_ms\":%.9f,\"clock\":\"%s\",\"build_info\":%s,"
           "\"lookup_scope\":\"hash_table_get plus hit/miss and byte checksum accumulation; batched clock\","
           "\"insertion_scope\":\"hash_table_set duplicate search, allocation, copying and synchronous rehash; per-operation clocks included in total\","
           "\"execution_order\":\"single selected structure per process; caller alternates processes\"",
           data->attempts, data->modulus, data->initial_concentrated ? "true" : "false",
           data->final_concentrated ? "true" : "false", data->generation_ms, MEASUREMENT_CLOCK_NAME, BUILD_INFO_JSON);
    print_clock_overhead();
    print_clock_properties();
    printf("}");
}

static void print_prepared(const bucket_bench_config *config, const bucket_dataset *data)
{
    printf("{\"version\":1,");
    print_config(config);
    printf(",\"keys\":[");
    for (uint32_t i = 0; i < config->count * 2; i++) {
        printf("%s\"%s\"", i ? "," : "", data->records[i].key);
    }
    printf("],\"values\":[");
    for (uint32_t i = 0; i < config->count * 2; i++) {
        printf("%s\"%s\"", i ? "," : "", data->records[i].value);
    }
    printf("],\"query_ids\":[");
    for (uint32_t i = 0; i < config->queries; i++) {
        printf("%s%" PRIu32, i ? "," : "", data->query_ids[i]);
    }
    printf("],");
    print_metadata(data);
    puts("}");
}

static uint64_t value_checksum(const void *value, size_t length)
{
    const unsigned char *bytes = value;
    uint64_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum = checksum * 33 + bytes[i];
    }
    return checksum;
}

static int run_round(const bucket_bench_config *config, const bucket_dataset *data, bucket_round *result)
{
    hash_table_t *table = NULL;
    result->samples = calloc(config->count, sizeof(*result->samples));
    if (!result->samples || hash_table_create_config(&config->table, &table) != HASH_TABLE_OK) {
        return 0;
    }
    uint64_t start = measurement_ticks();
    for (uint32_t i = 0; i < config->count; i++) {
        const bucket_record *record = &data->records[i];
        const size_t previous_buckets = hash_table_bucket_count(table);
        const uint64_t operation_start = measurement_ticks();
        const hash_table_status status = hash_table_set(table, record->key, record->key_length,
                                                        record->value, record->value_length, NULL);
        const uint64_t operation_end = measurement_ticks();
        result->samples[i].elapsed_ms = measurement_ms(operation_start, operation_end);
        result->samples[i].ok = status == HASH_TABLE_OK;
        result->samples[i].rehash = previous_buckets != hash_table_bucket_count(table);
        result->attempted_insertions++;
        if (status != HASH_TABLE_OK) {
            result->insertion_failures++;
            break;
        }
        result->insertion_successes++;
    }
    result->insertion_ms = measurement_ms(start, measurement_ticks());
    if (result->insertion_failures) {
        result->description = hash_table_describe(table);
        hash_table_destroy(table);
        return 0;
    }
    int verified = hash_table_validate(table);
    for (uint32_t i = 0; i < config->count * 2; i++) {
        const bucket_record *record = &data->records[i];
        const void *value;
        size_t length;
        const hash_table_status status = hash_table_get(table, record->key, record->key_length, &value, &length);
        if (i < config->count) {
            verified &= status == HASH_TABLE_OK;
            if (status == HASH_TABLE_OK) {
                verified &= length == record->value_length && !memcmp(value, record->value, length);
            }
        } else {
            verified &= status == HASH_TABLE_ERR_NOT_FOUND;
        }
    }
    /* Warmup uses the exact query order and contributes no reported samples. */
    uint64_t warmup_checksum = 0;
    uint64_t expected_hits = 0, expected_checksum = 0;
    for (uint32_t i = 0; i < config->queries; i++) {
        const uint32_t id = data->query_ids[i];
        const bucket_record *record = &data->records[id];
        const void *value;
        size_t length;
        if (hash_table_get(table, record->key, record->key_length, &value, &length) == HASH_TABLE_OK) {
            warmup_checksum += value_checksum(value, length);
        }
        if (id < config->count) {
            expected_hits++;
            expected_checksum += value_checksum(record->value, record->value_length);
        }
    }
    verified &= warmup_checksum == expected_checksum;
    start = measurement_ticks();
    for (uint32_t i = 0; i < config->queries; i++) {
        const bucket_record *record = &data->records[data->query_ids[i]];
        const void *value;
        size_t length;
        const hash_table_status status = hash_table_get(table, record->key, record->key_length, &value, &length);
        if (status == HASH_TABLE_OK) {
            result->hits++;
            result->checksum += value_checksum(value, length);
        } else if (status == HASH_TABLE_ERR_NOT_FOUND) {
            result->misses++;
        }
    }
    result->lookup_ms = measurement_ms(start, measurement_ticks());
    result->lookup_executed = 1;
    result->verified = verified && result->hits == expected_hits && result->misses == config->queries - expected_hits &&
                       result->checksum == expected_checksum && hash_table_validate(table);
    hash_table_stats stats;
    hash_table_snapshot(table, &stats, NULL, 0);
    if (config->collision) {
        result->verified &= stats.max_bucket_size == config->count && stats.nonempty_buckets == 1;
    }
    result->description = hash_table_describe(table);
    hash_table_destroy(table);
    return result->verified && result->description;
}

static void print_round(const bucket_bench_config *config, const bucket_round *result, uint32_t round)
{
    printf("{\"round\":%" PRIu32 ",\"bucket_structure\":\"%s\",\"insertion_ms\":%.9f,\"lookup_ms\":",
           round, hash_table_structure_name(config->table.bucket_structure), result->insertion_ms);
    if (result->lookup_executed) {
        printf("%.9f", result->lookup_ms);
    } else {
        printf("null");
    }
    printf(",\"hits\":%" PRIu64 ",\"misses\":%" PRIu64 ",\"checksum\":%" PRIu64
           ",\"verified\":%s,\"insertion_successes\":%" PRIu32 ",\"insertion_failures\":%" PRIu32
           ",\"insertion_samples\":[", result->hits, result->misses, result->checksum,
           result->verified ? "true" : "false", result->insertion_successes, result->insertion_failures);
    for (uint32_t i = 0; i < result->attempted_insertions; i++) {
        printf("%s{\"index\":%" PRIu32 ",\"ok\":%s,\"rehash\":%s,\"elapsed_ms\":%.9f}", i ? "," : "", i,
               result->samples[i].ok ? "true" : "false", result->samples[i].rehash ? "true" : "false", result->samples[i].elapsed_ms);
    }
    /* The description is owned, so remove only its outer closing brace. */
    char *stats = strstr(result->description, "\"stats\":");
    const size_t length = strlen(stats);
    printf("],%.*s}", (int)(length - 1), stats);
}

int main(int argc, char **argv)
{
    bucket_bench_config config;
    if (!read_options(argc, argv, &config)) {
        fputs("Invalid bucket benchmark options\n", stderr);
        return EXIT_FAILURE;
    }
    bucket_dataset data;
    if (!prepare_dataset(&config, &data)) {
        free_dataset(&data);
        fputs("Dataset preparation failed\n", stderr);
        return EXIT_FAILURE;
    }
    if (config.prepare_only) {
        print_prepared(&config, &data);
        free_dataset(&data);
        return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    bucket_round *rounds = calloc(config.rounds, sizeof(*rounds));
    int ok = rounds != NULL;
    uint32_t completed_rounds = 0;
    for (uint32_t i = 0; ok && i < config.rounds; i++) {
        ok = run_round(&config, &data, &rounds[i]);
        if (rounds[i].description) {
            completed_rounds++;
        }
    }
    if (completed_rounds) {
        printf("{\"version\":1,\"ok\":%s,", ok ? "true" : "false");
        print_config(&config);
        printf(",");
        print_metadata(&data);
        printf(",\"rounds\":[");
        for (uint32_t i = 0; i < completed_rounds; i++) {
            if (i) {
                printf(",");
            }
            print_round(&config, &rounds[i], i);
        }
        puts("]}");
    }
    if (!ok) {
        fputs("Bucket measurement allocation/verification failed; partial JSON is not a successful result\n", stderr);
    }
    if (rounds) {
        for (uint32_t i = 0; i < config.rounds; i++) {
            free(rounds[i].samples);
            free(rounds[i].description);
        }
    }
    free(rounds);
    free_dataset(&data);
    return ok && !ferror(stdout) ? EXIT_SUCCESS : EXIT_FAILURE;
}
