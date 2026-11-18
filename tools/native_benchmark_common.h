#ifndef NATIVE_BENCHMARK_COMMON_H
#define NATIVE_BENCHMARK_COMMON_H
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include "measurement_clock.h"
#include "build_info.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BENCH_BUILD_FLAGS
#define BENCH_BUILD_FLAGS "not supplied"
#endif

typedef struct {
    uint32_t count, queries, rounds, seed, hit_percent;
    const char *mode, *dir, *schema;
} bench_config;
typedef struct {
    uint32_t id;
    int hit;
    size_t key_length;
    char key[16], sql[64], value[32];
    int64_t matched_id;
    unsigned char matched_key[8];
} bench_query;
typedef struct {
    double elapsed_ms;
    uint64_t hits, misses, checksum;
} bench_result;

static int bench_number(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *out)
{
    uint64_t value = 0;
    if (!*text) {
        return 0;
    }
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || value > (maximum - (uint32_t)(*text - '0')) / 10U ||
            (uint32_t)(*text - '0') > maximum) {
            return 0;
        }
        value = value * 10U + (uint32_t)(*text - '0');
        if (value > maximum) {
            return 0;
        }
    }
    if (value < minimum) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static int bench_options(int argc, char **argv, int rdbms, bench_config *config)
{
    unsigned seen = 0;
    *config = (bench_config){100, 1000, 3, 1, 80, rdbms ? "bptree" : "hash", NULL, NULL};
    for (int i = 1; i < argc; i += 2) {
        int option = -1;
        static const char *const names[] = {
            "--count", "--queries", "--rounds", "--seed", "--hit-percent",
            "--mode", "--data-dir", "--schema"
        };
        uint32_t *const numbers[] = {
            &config->count, &config->queries, &config->rounds, &config->seed, &config->hit_percent
        };
        const uint32_t maxima[] = {10000, 1000000, 20, UINT32_MAX, 100};
        for (int j = 0; j < (rdbms ? 8 : 5); j++) {
            if (!strcmp(argv[i], names[j])) {
                option = j;
            }
        }
        if (option < 0 || i + 1 == argc || (seen & (1U << option))) {
            return 0;
        }
        seen |= 1U << option;
        if (option < 5) {
            if (!bench_number(argv[i + 1], option < 3 ? 1U : 0U, maxima[option], numbers[option])) {
                return 0;
            }
        } else if (option == 5) {
            config->mode = argv[i + 1];
            if (strcmp(config->mode, "scan") && strcmp(config->mode, "bptree")) {
                return 0;
            }
        } else if (option == 6) {
            config->dir = argv[i + 1];
        } else {
            config->schema = argv[i + 1];
        }
    }
    return !rdbms || (config->dir && *config->dir && config->schema && *config->schema);
}

static uint32_t bench_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

/* Canonical representation for hash keys, scalar payloads and checksums. */
static void bench_encode_scalar(unsigned char bytes[8], int64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = (unsigned char)((uint64_t)value >> (8 * i));
    }
}

static bench_query *bench_queries(const bench_config *config)
{
    bench_query *queries = calloc(config->queries, sizeof(*queries));
    uint32_t state = config->seed;
    const uint32_t hits = (uint32_t)((uint64_t)config->queries * config->hit_percent / 100U);
    if (!queries) {
        return NULL;
    }
    for (uint32_t i = 0; i < config->queries; i++) {
        queries[i].hit = i < hits;
        queries[i].id = 1U + bench_random(&state) % config->count + (i < hits ? 0U : config->count);
    }
    for (uint32_t i = config->queries; i > 1; i--) {
        const uint32_t j = bench_random(&state) % i;
        const bench_query swap = queries[i - 1];
        queries[i - 1] = queries[j];
        queries[j] = swap;
    }
    for (uint32_t i = 0; i < config->queries; i++) {
        snprintf(queries[i].key, sizeof(queries[i].key), "%" PRIu32, queries[i].id);
        queries[i].key_length = strlen(queries[i].key);
        queries[i].matched_id = queries[i].id;
        bench_encode_scalar(queries[i].matched_key, queries[i].matched_id);
        snprintf(queries[i].sql, sizeof(queries[i].sql), "SELECT * FROM records WHERE id = %" PRIu32 ";", queries[i].id);
        snprintf(queries[i].value, sizeof(queries[i].value), "value-%" PRIu32, queries[i].id);
    }
    return queries;
}

static uint64_t bench_hash(uint64_t hash, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static void bench_record(bench_result *result, int hit, const void *data, size_t length)
{
    const unsigned char marker = (unsigned char)hit;
    result->checksum = bench_hash(result->checksum, &marker, 1);
    if (hit) {
        result->hits++;
        result->checksum = bench_hash(result->checksum, data, length);
    } else {
        result->misses++;
    }
}

typedef struct {
    int found;
    int64_t value;
} matched_sample;

static int bench_verify_matched(const bench_query *queries, uint32_t count,
                                const matched_sample *samples, bench_result *result)
{
    bench_result expected = {0, 0, 0, UINT64_C(14695981039346656037)};
    result->hits = result->misses = 0;
    result->checksum = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < count; i++) {
        unsigned char actual_bytes[8], expected_bytes[8];
        const int64_t value = (int64_t)queries[i].id * 3 + 7;
        if (samples[i].found != queries[i].hit ||
            (queries[i].hit && samples[i].value != value)) {
            return 0;
        }
        bench_encode_scalar(actual_bytes, samples[i].value);
        bench_encode_scalar(expected_bytes, value);
        bench_record(result, samples[i].found, actual_bytes, 8);
        bench_record(&expected, queries[i].hit, expected_bytes, 8);
    }
    return result->checksum == expected.checksum && result->hits == expected.hits &&
           result->misses == expected.misses;
}

static void bench_json_string(const char *text)
{
    putchar('"');
    for (; *text; text++) {
        const unsigned char byte = (unsigned char)*text;
        if (byte == '"' || byte == '\\') {
            putchar('\\');
            putchar(byte);
        } else if (byte < 32) {
            printf("\\u%04x", byte);
        } else {
            putchar(byte);
        }
    }
    putchar('"');
}

static void bench_print_result(const bench_result *result, uint32_t queries)
{
    printf("{\"elapsed_ms\":%.9f,\"operations\":%" PRIu32 ",\"hits\":%" PRIu64
           ",\"misses\":%" PRIu64 ",\"checksum\":\"%016" PRIx64 "\",\"verified\":true}",
           result->elapsed_ms, queries, result->hits, result->misses, result->checksum);
}

static void bench_output(const char *system, const bench_config *config, double setup_ms,
                         bench_result results[20][3], const char *storage, const char *flush,
                         const char *full_scope, const char *pure_scope)
{
    printf("{\"version\":1,\"system\":\"%s\",\"mode\":\"%s\",\"config\":{\"count\":%" PRIu32
           ",\"queries\":%" PRIu32 ",\"rounds\":%" PRIu32 ",\"seed\":%" PRIu32 ",\"hit_percent\":%" PRIu32
           "},\"setup_ms\":%.9f,\"rounds\":[", system, config->mode, config->count,
           config->queries, config->rounds, config->seed, config->hit_percent, setup_ms);
    for (uint32_t round = 0; round < config->rounds; round++) {
        printf("%s{\"round\":%" PRIu32 ",\"order\":%s,\"full_lookup\":", round ? "," : "", round + 1,
               !pure_scope ? "[\"full_lookup\"]" : round % 2 ?
               "[\"pure_lookup\",\"full_lookup\"]" : "[\"full_lookup\",\"pure_lookup\"]");
        bench_print_result(&results[round][0], config->queries);
        printf(",\"pure_lookup\":");
        if (pure_scope) {
            bench_print_result(&results[round][1], config->queries);
        } else {
            printf("null");
        }
        printf(",\"matched_lookup\":");
        if (pure_scope) {
            bench_print_result(&results[round][2], config->queries);
        } else {
            printf("null");
        }
        printf(",\"matched_order\":%s}", !pure_scope ? "[\"full_lookup\"]" : round % 2 ?
               "[\"matched_lookup\",\"pure_lookup\",\"full_lookup\"]" :
               "[\"full_lookup\",\"pure_lookup\",\"matched_lookup\"]");
    }
    printf("],\"metadata\":{\"clock\":\"%s\",\"build_flags\":", MEASUREMENT_CLOCK_NAME);
    bench_json_string(BENCH_BUILD_FLAGS);
    printf(",\"compiler\":");
    bench_json_string(__VERSION__);
    printf(",\"cpu\":");
    bench_json_string(getenv("PROCESSOR_IDENTIFIER") ? getenv("PROCESSOR_IDENTIFIER") : "not reported by environment");
    printf(",\"build_timestamp\":");
    bench_json_string(BUILD_TIMESTAMP);
    printf("%s", ",\"build_info\":" BUILD_INFO_JSON);
#ifdef _WIN32
    printf(",\"platform\":\"Windows\"");
#else
    printf(",\"platform\":\"POSIX\"");
#endif
    printf(",\"pointer_bits\":%u,\"storage\":", (unsigned)(sizeof(void *) * 8));
    bench_json_string(storage);
    printf(",\"flush\":");
    bench_json_string(flush);
    printf(",\"full_lookup_scope\":");
    bench_json_string(full_scope);
    printf(",\"pure_lookup_scope\":");
    if (pure_scope) {
        bench_json_string(pure_scope);
    } else {
        printf("null");
    }
    printf(",\"matched_structure_comparison\":%s,\"matched_lookup_scope\":", pure_scope ? "true" : "false");
    if (pure_scope) {
        bench_json_string("in-memory int64 key -> int64 scalar lookup; loop, found/scalar capture included; hash uses eight little-endian key/value bytes plus scalar decode; B+ uses integer slots; no CSV, TTL, SQL, allocation, checksum or verification");
    } else {
        printf("null");
    }
    if (pure_scope) {
        printf(",\"matched_key_type\":\"int64, hash eight little-endian bytes; B+ int64\","
               "\"matched_value_type\":\"int64 scalar; checksum eight little-endian bytes\","
               "\"matched_dataset\":\"keys 1..count, value=key*3+7; common seeded query sequence; absent keys count+1..2*count\"");
    } else {
        printf(",\"matched_key_type\":null,\"matched_value_type\":null,\"matched_dataset\":null");
    }
    printf(",\"equivalent_structure_comparison\":false,"
           "\"comparison_caveat\":\"Old pure_lookup: hash returns borrowed value/length for decimal byte keys; B+ returns CSV offset for integer IDs. Different work, not equivalent structures. New matched_lookup is separate. Full engine scopes also differ.\","
           "\"query_distribution\":\"32-bit LCG, modulo N present/absent IDs, deterministic Fisher-Yates shuffle; minor modulo bias\","
           "\"hit_policy\":\"floor(queries * hit_percent / 100)\","
           "\"warmup_queries_per_layer\":%" PRIu32 ","
           "\"cache_policy\":\"warmup; OS caches not evicted; not cold disk\","
           "\"hash_seed_policy\":\"Redis pure and matched tables seeded from config seed; Redis store uses OS randomness; query list and checksums deterministic\","
           "\"timed_overhead\":\"loop and preallocated actual-result capture; no per-query clocks\","
           "\"verification_scope\":\"outside timing: independently compare every result, then FNV-1a actual hit markers and payload bytes; pure B+ uses little-endian CSV offsets; matched uses independently derived key*3+7 little-endian scalars\"}}\n",
           config->queries < 64 ? config->queries : 64);
}
#endif
