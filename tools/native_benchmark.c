#include "native_benchmark_common.h"
#include "hash_table.h"
#include "store.h"

typedef struct {
    const void *value;
    size_t length;
    int status;
} lookup_sample;

static int run_layer(store_t *store, const hash_table_t *table, int pure,
                     const bench_query *queries, uint32_t count,
                     lookup_sample *samples, bench_result *result)
{
    const uint64_t start = measurement_ticks();
    for (uint32_t i = 0; i < count; i++) {
        samples[i].value = NULL;
        samples[i].length = 0;
        if (pure) {
            samples[i].status = hash_table_get(table, queries[i].key, queries[i].key_length,
                                               &samples[i].value, &samples[i].length);
        } else {
            samples[i].status = store_get(store, queries[i].key, queries[i].key_length, 0,
                                          &samples[i].value, &samples[i].length);
        }
    }
    const uint64_t end = measurement_ticks();
    *result = (bench_result){measurement_ms(start, end), 0, 0, UINT64_C(14695981039346656037)};
    for (uint32_t i = 0; i < count; i++) {
        const int expected_status = queries[i].hit ? 0 : pure ? HASH_TABLE_ERR_NOT_FOUND : STORE_NOT_FOUND;
        if (samples[i].status != expected_status ||
            (queries[i].hit && (samples[i].length != strlen(queries[i].value) ||
             memcmp(samples[i].value, queries[i].value, samples[i].length)))) {
            return 0;
        }
        bench_record(result, queries[i].hit, samples[i].value, samples[i].length);
    }
    return 1;
}

static int run_matched(const hash_table_t *table, const bench_query *queries,
                        uint32_t count, matched_sample *samples, bench_result *result)
{
    const uint64_t start = measurement_ticks();
    for (uint32_t i = 0; i < count; i++) {
        const void *value = NULL;
        size_t length = 0;
        const int status = hash_table_get(table, queries[i].matched_key, 8, &value, &length);
        samples[i].found = status == HASH_TABLE_OK ? 1 : status == HASH_TABLE_ERR_NOT_FOUND ? 0 : -1;
        samples[i].value = 0;
        if (status == HASH_TABLE_OK) {
            if (length != 8) {
                samples[i].found = -1;
            } else {
                const unsigned char *bytes = value;
                uint64_t scalar = 0;
                for (unsigned j = 0; j < 8; j++) {
                    scalar |= (uint64_t)bytes[j] << (8 * j);
                }
                samples[i].value = (int64_t)scalar;
            }
        }
    }
    const uint64_t end = measurement_ticks();
    result->elapsed_ms = measurement_ms(start, end);
    return bench_verify_matched(queries, count, samples, result);
}

int main(int argc, char **argv)
{
    bench_config config;
    store_t *store = NULL;
    hash_table_t *table = NULL;
    hash_table_t *matched_table = NULL;
    matched_sample *matched_samples = NULL;
    bench_query *queries = NULL;
    lookup_sample *samples = NULL;
    bench_result results[20][3] = {{{0}}}, warmup;
    uint8_t hash_seed[HASH_TABLE_SEED_SIZE];
    uint32_t state;
    int ok = 0;
    if (!bench_options(argc, argv, 0, &config)) {
        fprintf(stderr, "Invalid options: --count 1..%u --queries 1..%u --rounds 1..%u --seed uint32 --hit-percent 0..100\n",
                BENCH_COUNT_MAX, BENCH_QUERIES_MAX, BENCH_ROUNDS_MAX);
        return 2;
    }
    const uint64_t start = measurement_ticks();
    state = config.seed;
    for (size_t i = 0; i < sizeof(hash_seed); i++) {
        hash_seed[i] = (uint8_t)(bench_random(&state) >> 24);
    }
    queries = bench_queries(&config);
    samples = calloc(config.queries, sizeof(*samples));
    matched_samples = calloc(config.queries, sizeof(*matched_samples));
    if (!queries || !samples || !matched_samples ||
        hash_table_create_with_seed(16, hash_seed, &matched_table) != HASH_TABLE_OK || store_create(16, &store) != STORE_OK ||
        hash_table_create_with_seed(16, hash_seed, &table) != HASH_TABLE_OK) {
        goto finish;
    }
    for (uint32_t i = 1; i <= config.count; i++) {
        char key[16], value[32];
        unsigned char matched_key[8], matched_value[8];
        bench_encode_scalar(matched_key, i);
        bench_encode_scalar(matched_value, (int64_t)i * 3 + 7);
        if (hash_table_set(matched_table, matched_key, 8, matched_value, 8, NULL) != HASH_TABLE_OK) {
            goto finish;
        }
        snprintf(key, sizeof(key), "%" PRIu32, i);
        snprintf(value, sizeof(value), "value-%" PRIu32, i);
        if (store_put(store, key, strlen(key), value, strlen(value), 0, 0) != STORE_OK ||
            hash_table_set(table, key, strlen(key), value, strlen(value), NULL) != HASH_TABLE_OK) {
            goto finish;
        }
    }
    for (int layer = 0; layer < 2; layer++) {
        if (!run_layer(store, table, layer, queries, config.queries < 64 ? config.queries : 64, samples, &warmup)) {
            goto finish;
        }
    }
    if (!run_matched(matched_table, queries, config.queries < 64 ? config.queries : 64,
                     matched_samples, &warmup)) {
        goto finish;
    }
    const double setup_ms = measurement_ms(start, measurement_ticks());
    for (uint32_t round = 0; round < config.rounds; round++) {
        for (int pass = 0; pass < 3; pass++) {
            const int layer = round % 2 ? 2 - pass : pass;
            const int verified = layer == 2 ?
                run_matched(matched_table, queries, config.queries, matched_samples, &results[round][layer]) :
                run_layer(store, table, layer, queries, config.queries, samples, &results[round][layer]);
            if (!verified || (round && results[round][layer].checksum != results[0][layer].checksum)) {
                goto finish;
            }
        }
    }
    bench_output("redis", &config, setup_ms, results, "in-memory; no persistence", "none",
                 "store_get: SipHash lookup, TTL check, borrowed value/length; result capture, no HTTP/body copy",
                 "hash_table_get: decimal key bytes -> borrowed value/length; no TTL; capture included, no allocation");
    ok = fflush(stdout) == 0 && !ferror(stdout);
finish:
    if (!ok) {
        fprintf(stderr, "Native Redis benchmark failed: allocation, setup, verification or output failure\n");
    }
    free(matched_samples);
    hash_table_destroy(matched_table);
    free(samples);
    free(queries);
    hash_table_destroy(table);
    store_destroy(store);
    return ok ? 0 : 1;
}
