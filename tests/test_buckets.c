#include "hash_table.h"
#include "store.h"
#include "hash_table_test_access.h"
#include "test_allocator.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void model_operations(hash_table_bucket_structure mode, int collisions)
{
    uint8_t seed[16] = {0};
    const hash_table_config config = {1, mode, seed};
    hash_table_t *table = NULL;
    assert(hash_table_create_config(&config, &table) == HASH_TABLE_OK);
    if (collisions) {
        assert(hash_table_test_force_collisions(table));
    }
    uint32_t model[257] = {0};
    uint32_t state = 42;
    for (unsigned step = 0; step < 20000; step++) {
        const uint32_t id = next_random(&state) % 257;
        unsigned char key[4] = {(unsigned char)id, 0, (unsigned char)(id >> 8), 0xff};
        /* id 0 exercises empty key; embedded NUL and differing lengths otherwise. */
        const size_t key_len = id ? sizeof(key) : 0;
        const unsigned operation = next_random(&state) % 3;
        if (operation == 0) {
            const uint32_t value = step + 1;
            hash_table_set_outcome outcome;
            assert(hash_table_set(table, key, key_len, &value, sizeof(value), &outcome) == HASH_TABLE_OK);
            assert(outcome == (model[id] ? HASH_TABLE_SET_REPLACED : HASH_TABLE_SET_INSERTED));
            model[id] = value;
        } else if (operation == 1) {
            assert(hash_table_delete(table, key, key_len) == (model[id] ? HASH_TABLE_OK : HASH_TABLE_ERR_NOT_FOUND));
            model[id] = 0;
        } else {
            const void *value = NULL;
            size_t length = 0;
            assert(hash_table_get(table, key, key_len, &value, &length) == (model[id] ? HASH_TABLE_OK : HASH_TABLE_ERR_NOT_FOUND));
            if (model[id]) {
                assert(length == sizeof(uint32_t) && !memcmp(value, &model[id], length));
            }
        }
        assert(hash_table_validate(table));
    }
    for (uint32_t id = 0; id < 257; id++) {
        unsigned char key[4] = {(unsigned char)id, 0, (unsigned char)(id >> 8), 0xff};
        if (model[id]) {
            assert(hash_table_delete(table, key, id ? sizeof(key) : 0) == HASH_TABLE_OK);
            assert(hash_table_validate(table));
        }
    }
    assert(hash_table_size(table) == 0);
    hash_table_destroy(table);
}

static void failures_and_aliases(hash_table_bucket_structure mode)
{
    uint8_t seed[16] = {0};
    const hash_table_config config = {1, mode, seed};
    for (size_t failure = 1; failure <= 4; failure++) {
        hash_table_t *table = NULL;
        assert(hash_table_create_config(&config, &table) == HASH_TABLE_OK);
        assert(hash_table_set(table, "a", 1, "abc", 3, NULL) == HASH_TABLE_OK);
        const void *borrowed;
        size_t length;
        assert(hash_table_get(table, "a", 1, &borrowed, &length) == HASH_TABLE_OK);
        hash_table_set_outcome outcome = HASH_TABLE_SET_REPLACED;
        const long live = test_allocator_live_count();
        test_allocator_fail_nth(failure);
        assert(hash_table_set(table, borrowed, length, borrowed, length, &outcome) == HASH_TABLE_ERR_NO_MEMORY);
        test_allocator_fail_nth(0);
        assert(outcome == HASH_TABLE_SET_REPLACED && hash_table_size(table) == 1);
        assert(hash_table_bucket_count(table) == 1 && hash_table_validate(table));
        assert(test_allocator_live_count() == live);
        assert(hash_table_get(table, "a", 1, &borrowed, &length) == HASH_TABLE_OK);
        assert(length == 3 && !memcmp(borrowed, "abc", 3));
        assert(hash_table_set(table, borrowed, length, borrowed, length, NULL) == HASH_TABLE_OK);
        assert(hash_table_validate(table));
        test_allocator_fail_nth(1);
        assert(hash_table_set(table, "a", 1, "bad", 3, &outcome) == HASH_TABLE_ERR_NO_MEMORY);
        test_allocator_fail_nth(0);
        assert(hash_table_get(table, "a", 1, &borrowed, &length) == HASH_TABLE_OK);
        assert(!memcmp(borrowed, "abc", 3));
        hash_table_destroy(table);
    }
}

static void exact_growth_rollback(hash_table_bucket_structure mode)
{
    uint8_t seed[16] = {0};
    const hash_table_config config = {4, mode, seed};
    hash_table_t *table;
    assert(hash_table_create_config(&config, &table) == HASH_TABLE_OK);
    assert(hash_table_test_force_collisions(table));
    const unsigned char key[] = {0, 0xff, 1};
    for (size_t length = 0; length <= 3; length++) {
        assert(hash_table_set(table, key, length, key, length, NULL) == HASH_TABLE_OK);
    }
    hash_table_test_link before[4], after[4];
    assert(hash_table_test_collect_links(table, before, 4) == 4);
    char *description = hash_table_describe(table);
    assert(description);
    for (size_t failure = 1; failure <= 4; failure++) {
        hash_table_set_outcome outcome = HASH_TABLE_SET_REPLACED;
        test_allocator_fail_nth(failure);
        assert(hash_table_set(table, "new", 3, "value", 5, &outcome) == HASH_TABLE_ERR_NO_MEMORY);
        test_allocator_fail_nth(0);
        assert(outcome == HASH_TABLE_SET_REPLACED);
        assert(hash_table_test_collect_links(table, after, 4) == 4);
        assert(!memcmp(before, after, sizeof(before)));
        char *current = hash_table_describe(table);
        assert(current && !strcmp(current, description));
        free(current);
        assert(hash_table_validate(table));
        for (size_t length = 0; length <= 3; length++) {
            const void *value;
            size_t value_length;
            assert(hash_table_get(table, key, length, &value, &value_length) == HASH_TABLE_OK);
            assert(value_length == length && (!length || !memcmp(value, key, length)));
        }
    }
    free(description);
    hash_table_destroy(table);
}

static void ttl_operations(hash_table_bucket_structure mode)
{
    uint8_t seed[16] = {0};
    const hash_table_config config = {1, mode, seed};
    store_t *store;
    assert(store_create_config(&config, &store) == STORE_OK);
    for (unsigned i = 0; i < 64; i++) {
        assert(store_put(store, &i, sizeof(i), &i, sizeof(i), 10 + i, 100) == STORE_OK);
    }
    for (unsigned i = 0; i < 64; i++) {
        const void *value;
        size_t length;
        assert(store_get(store, &i, sizeof(i), 109 + i, &value, &length) == STORE_OK);
        assert(length == sizeof(i) && !memcmp(value, &i, length));
        assert(store_get(store, &i, sizeof(i), 110 + i, &value, &length) == STORE_EXPIRED);
        assert(store_delete(store, &i, sizeof(i), 110 + i) == STORE_NOT_FOUND);
    }
    assert(store_put(store, "a", 1, "old", 3, 5, 100) == STORE_OK);
    for (size_t failure = 1; failure <= 2; failure++) {
        test_allocator_fail_nth(failure);
        assert(store_put(store, "a", 1, "new", 3, 0, 101) == STORE_NO_MEMORY);
        test_allocator_fail_nth(0);
        const void *value;
        size_t length;
        assert(store_get(store, "a", 1, 104, &value, &length) == STORE_OK);
        assert(length == 3 && !memcmp(value, "old", 3));
    }
    const void *value;
    size_t length;
    assert(store_get(store, "a", 1, 105, &value, &length) == STORE_EXPIRED);
    store_destroy(store);
}

int main(void)
{
    for (int mode = 0; mode < 2; mode++) {
        model_operations((hash_table_bucket_structure)mode, 0);
        model_operations((hash_table_bucket_structure)mode, 1);
        failures_and_aliases((hash_table_bucket_structure)mode);
        exact_growth_rollback((hash_table_bucket_structure)mode);
        ttl_operations((hash_table_bucket_structure)mode);
    }
    assert(test_allocator_live_count() == 0 && test_allocator_error_count() == 0);
    puts("Bucket tests: 80000 model operations, collision/rehash/delete invariants, OOM/alias/TTL passed; allocator 0 live/errors");
    return 0;
}
