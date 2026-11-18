#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hash_table.h"
#include "hash_table_test_access.h"
#include "key_hash.h"
#include "test_allocator.h"
#include "vectors.h"

static int current_test_failed = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
            current_test_failed = 1;                                         \
        }                                                                    \
    } while (0)

static const uint8_t TEST_SEED[HASH_TABLE_SEED_SIZE] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static hash_table_t *create_seeded_table(size_t bucket_count)
{
    hash_table_t *table = NULL;
    CHECK(hash_table_create_with_seed(bucket_count, TEST_SEED, &table) == HASH_TABLE_OK);
    return table;
}

static hash_table_status set_text(hash_table_t *table, const char *key, const char *value)
{
    return hash_table_set(table, key, strlen(key), value, strlen(value), NULL);
}

/* 키의 값이 expected 문자열과 바이트 단위로 같은지 확인한다. */
static int has_text_value(const hash_table_t *table, const char *key, const char *expected)
{
    const void *value = NULL;
    size_t value_len = 0;

    if (hash_table_get(table, key, strlen(key), &value, &value_len) != HASH_TABLE_OK) {
        return 0;
    }
    return value_len == strlen(expected) && memcmp(value, expected, value_len) == 0;
}

static int has_key(const hash_table_t *table, const char *key)
{
    const void *value = NULL;
    size_t value_len = 0;
    return hash_table_get(table, key, strlen(key), &value, &value_len) == HASH_TABLE_OK;
}

#define MAX_TEST_LINKS 128
#define TEST_KEY_SIZE 16

static size_t expected_bucket_index(const void *key, size_t key_len, size_t bucket_count)
{
    return (size_t)(key_hash_siphash24(TEST_SEED, key, key_len) % (uint64_t)bucket_count);
}

/*
 * 고정 시드에서 같은 버킷으로 가는 키를 count개 고른다. 버킷 1개로 충돌을 강제하던 방식은
 * 자동 확장 때문에 더 이상 충돌을 보장하지 않으므로, 충분한 버킷 수에서 해시로 직접 고른다.
 */
static size_t find_colliding_keys(size_t bucket_count, char keys[][TEST_KEY_SIZE], size_t count)
{
    size_t found = 0;
    size_t target = 0;

    for (int i = 0; found < count && i < 100000; i++) {
        char candidate[TEST_KEY_SIZE];
        snprintf(candidate, sizeof(candidate), "c%d", i);
        const size_t index = expected_bucket_index(candidate, strlen(candidate), bucket_count);
        if (found == 0) {
            target = index;
        }
        if (index == target) {
            memcpy(keys[found], candidate, sizeof(candidate));
            found++;
        }
    }
    return found;
}

/* 모든 엔트리가 현재 버킷 수 기준의 올바른 버킷에 정확히 한 번씩 있는지 확인한다. */
static int has_consistent_layout(const hash_table_t *table)
{
    hash_table_test_link links[MAX_TEST_LINKS];
    const size_t count = hash_table_test_collect_links(table, links, MAX_TEST_LINKS);

    if (count != hash_table_size(table) || count > MAX_TEST_LINKS) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (links[i].bucket_index != expected_bucket_index(links[i].key, links[i].key_len,
                                                           hash_table_bucket_count(table))) {
            return 0;
        }
        for (size_t j = i + 1; j < count; j++) {
            const int same_key = links[i].key_len == links[j].key_len
                && memcmp(links[i].key, links[j].key, links[i].key_len) == 0;
            if (links[i].entry == links[j].entry || same_key) {
                return 0;
            }
        }
    }
    return 1;
}

static int same_links(const hash_table_test_link *a, const hash_table_test_link *b, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (a[i].bucket_index != b[i].bucket_index || a[i].entry != b[i].entry || a[i].next != b[i].next
            || a[i].key != b[i].key || a[i].key_len != b[i].key_len) {
            return 0;
        }
    }
    return 1;
}

static void test_siphash_matches_official_vectors(void)
{
    uint8_t seed[KEY_HASH_SEED_SIZE];
    uint8_t input[64];

    for (int i = 0; i < KEY_HASH_SEED_SIZE; i++) {
        seed[i] = (uint8_t)i;
    }
    for (int i = 0; i < 64; i++) {
        input[i] = (uint8_t)i;
    }
    for (int len = 0; len < 64; len++) {
        uint64_t expected = 0;
        for (int b = 7; b >= 0; b--) {
            expected = (expected << 8) | vectors_sip64[len][b];
        }
        CHECK(key_hash_siphash24(seed, input, (size_t)len) == expected);
    }
}

static void test_create_and_destroy_empty(void)
{
    hash_table_t *table = create_seeded_table(HASH_TABLE_DEFAULT_BUCKET_COUNT);
    CHECK(table != NULL);
    CHECK(hash_table_size(table) == 0);
    CHECK(hash_table_bucket_count(table) == HASH_TABLE_DEFAULT_BUCKET_COUNT);
    hash_table_destroy(table);

    hash_table_t *random_table = NULL;
    CHECK(hash_table_create(1, &random_table) == HASH_TABLE_OK);
    CHECK(random_table != NULL);
    CHECK(set_text(random_table, "k", "v") == HASH_TABLE_OK);
    CHECK(has_text_value(random_table, "k", "v"));
    hash_table_destroy(random_table);

    hash_table_destroy(NULL);
}

static void test_set_then_get(void)
{
    hash_table_t *table = create_seeded_table(HASH_TABLE_DEFAULT_BUCKET_COUNT);
    hash_table_set_outcome outcome = HASH_TABLE_SET_REPLACED;

    CHECK(hash_table_set(table, "name", 4, "redis", 5, &outcome) == HASH_TABLE_OK);
    CHECK(outcome == HASH_TABLE_SET_INSERTED);
    CHECK(hash_table_size(table) == 1);
    CHECK(has_text_value(table, "name", "redis"));
    hash_table_destroy(table);
}

static void test_missing_key_get_and_delete(void)
{
    hash_table_t *table = create_seeded_table(HASH_TABLE_DEFAULT_BUCKET_COUNT);
    const void *value = (const void *)&value;
    size_t value_len = 1234;

    CHECK(set_text(table, "present", "1") == HASH_TABLE_OK);
    CHECK(hash_table_get(table, "absent", 6, &value, &value_len) == HASH_TABLE_ERR_NOT_FOUND);
    CHECK(value == (const void *)&value && value_len == 1234);
    CHECK(hash_table_delete(table, "absent", 6) == HASH_TABLE_ERR_NOT_FOUND);
    CHECK(hash_table_size(table) == 1);
    hash_table_destroy(table);
}

static void test_replace_changes_value_and_keeps_size(void)
{
    hash_table_t *table = create_seeded_table(HASH_TABLE_DEFAULT_BUCKET_COUNT);
    hash_table_set_outcome outcome = HASH_TABLE_SET_INSERTED;

    CHECK(set_text(table, "key", "old") == HASH_TABLE_OK);
    CHECK(hash_table_set(table, "key", 3, "new-longer", 10, &outcome) == HASH_TABLE_OK);
    CHECK(outcome == HASH_TABLE_SET_REPLACED);
    CHECK(hash_table_size(table) == 1);
    CHECK(has_text_value(table, "key", "new-longer"));
    hash_table_destroy(table);
}

static void test_distinct_keys_are_independent(void)
{
    hash_table_t *table = create_seeded_table(HASH_TABLE_DEFAULT_BUCKET_COUNT);

    CHECK(set_text(table, "a", "1") == HASH_TABLE_OK);
    CHECK(set_text(table, "ab", "2") == HASH_TABLE_OK);
    CHECK(set_text(table, "b", "3") == HASH_TABLE_OK);
    CHECK(set_text(table, "ab", "22") == HASH_TABLE_OK);
    CHECK(hash_table_delete(table, "a", 1) == HASH_TABLE_OK);
    CHECK(!has_key(table, "a"));
    CHECK(has_text_value(table, "ab", "22"));
    CHECK(has_text_value(table, "b", "3"));
    CHECK(hash_table_size(table) == 2);
    hash_table_destroy(table);
}

/* 같은 버킷으로 가는 키를 확장 없이 넣어 한 체인에서 조회, 교체, 삭제 경로를 탄다. */
static void test_colliding_chain_operations(void)
{
    char keys[6][TEST_KEY_SIZE];
    CHECK(find_colliding_keys(16, keys, 6) == 6);
    hash_table_t *table = create_seeded_table(16);

    for (int i = 0; i < 6; i++) {
        CHECK(set_text(table, keys[i], keys[i]) == HASH_TABLE_OK);
    }
    CHECK(hash_table_bucket_count(table) == 16);
    hash_table_test_link links[MAX_TEST_LINKS];
    CHECK(hash_table_test_collect_links(table, links, MAX_TEST_LINKS) == 6);
    for (int i = 1; i < 6; i++) {
        CHECK(links[i].bucket_index == links[0].bucket_index);
    }
    CHECK(set_text(table, keys[2], "replaced") == HASH_TABLE_OK);
    CHECK(hash_table_delete(table, keys[4], strlen(keys[4])) == HASH_TABLE_OK);
    CHECK(hash_table_size(table) == 5);
    for (int i = 0; i < 6; i++) {
        if (i == 4) {
            CHECK(!has_key(table, keys[i]));
        } else if (i == 2) {
            CHECK(has_text_value(table, keys[i], "replaced"));
        } else {
            CHECK(has_text_value(table, keys[i], keys[i]));
        }
    }
    CHECK(has_consistent_layout(table));
    hash_table_destroy(table);
}

/* 머리 삽입이므로 0, 1, 2 순서로 넣으면 체인은 2 -> 1 -> 0: 2가 머리, 1이 중간, 0이 꼬리이다. */
static void test_delete_head_middle_tail(void)
{
    char keys[3][TEST_KEY_SIZE];
    CHECK(find_colliding_keys(16, keys, 3) == 3);

    for (int removed = 0; removed < 3; removed++) {
        hash_table_t *table = create_seeded_table(16);
        for (int i = 0; i < 3; i++) {
            CHECK(set_text(table, keys[i], keys[i]) == HASH_TABLE_OK);
        }
        hash_table_test_link links[MAX_TEST_LINKS];
        CHECK(hash_table_test_collect_links(table, links, MAX_TEST_LINKS) == 3);
        CHECK(links[0].bucket_index == links[1].bucket_index && links[1].bucket_index == links[2].bucket_index);
        CHECK(links[0].next == links[1].entry && links[1].next == links[2].entry && links[2].next == NULL);
        /* 체인 순서는 공개 계약이 아니지만, 이 테스트가 세 위치를 모두 지나는지 확인하려고 구현 순서를 검사한다. */
        CHECK(memcmp(links[0].key, keys[2], strlen(keys[2])) == 0);
        CHECK(memcmp(links[2].key, keys[0], strlen(keys[0])) == 0);
        CHECK(hash_table_delete(table, keys[removed], strlen(keys[removed])) == HASH_TABLE_OK);
        CHECK(hash_table_size(table) == 2);
        for (int i = 0; i < 3; i++) {
            if (i == removed) {
                CHECK(!has_key(table, keys[i]));
            } else {
                CHECK(has_text_value(table, keys[i], keys[i]));
            }
        }
        CHECK(has_consistent_layout(table));
        hash_table_destroy(table);
    }
}

static void test_reinsert_after_deleting_last_entry(void)
{
    hash_table_t *table = create_seeded_table(1);
    hash_table_set_outcome outcome = HASH_TABLE_SET_REPLACED;

    CHECK(set_text(table, "only", "1") == HASH_TABLE_OK);
    CHECK(hash_table_delete(table, "only", 4) == HASH_TABLE_OK);
    CHECK(hash_table_size(table) == 0);
    CHECK(!has_key(table, "only"));
    CHECK(hash_table_set(table, "only", 4, "2", 1, &outcome) == HASH_TABLE_OK);
    CHECK(outcome == HASH_TABLE_SET_INSERTED);
    CHECK(hash_table_size(table) == 1);
    CHECK(has_text_value(table, "only", "2"));
    hash_table_destroy(table);
}

static void test_table_owns_copies_of_caller_buffers(void)
{
    hash_table_t *table = create_seeded_table(HASH_TABLE_DEFAULT_BUCKET_COUNT);
    char *key = malloc(4);
    char *value = malloc(4);

    memcpy(key, "key", 4);
    memcpy(value, "val", 4);
    CHECK(hash_table_set(table, key, 3, value, 3, NULL) == HASH_TABLE_OK);
    memcpy(key, "zzz", 4);
    memcpy(value, "XXX", 4);
    CHECK(has_text_value(table, "key", "val"));
    CHECK(!has_key(table, "zzz"));
    free(key);
    free(value);
    CHECK(has_text_value(table, "key", "val"));
    hash_table_destroy(table);
}

static void test_empty_binary_and_invalid_arguments(void)
{
    hash_table_t *table = create_seeded_table(HASH_TABLE_DEFAULT_BUCKET_COUNT);
    const uint8_t binary_key[3] = {0x00, 0xff, 0x00};
    const uint8_t binary_prefix[1] = {0x00};
    const uint8_t binary_value[2] = {0x00, 0x7f};
    const void *value = NULL;
    size_t value_len = 99;

    CHECK(hash_table_set(table, NULL, 0, NULL, 0, NULL) == HASH_TABLE_OK);
    CHECK(hash_table_get(table, "", 0, &value, &value_len) == HASH_TABLE_OK);
    CHECK(value != NULL && value_len == 0);

    CHECK(hash_table_set(table, binary_key, 3, binary_value, 2, NULL) == HASH_TABLE_OK);
    CHECK(hash_table_get(table, binary_key, 3, &value, &value_len) == HASH_TABLE_OK);
    CHECK(value_len == 2 && memcmp(value, binary_value, 2) == 0);
    CHECK(hash_table_get(table, binary_prefix, 1, &value, &value_len) == HASH_TABLE_ERR_NOT_FOUND);
    CHECK(hash_table_size(table) == 2);

    CHECK(hash_table_set(NULL, "k", 1, "v", 1, NULL) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_set(table, NULL, 1, "v", 1, NULL) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_set(table, "k", 1, NULL, 1, NULL) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_get(table, "k", 1, NULL, &value_len) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_get(table, "k", 1, &value, NULL) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_delete(table, NULL, 1) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_size(table) == 2);
    CHECK(hash_table_size(NULL) == 0);

    hash_table_t *out = (hash_table_t *)&out;
    CHECK(hash_table_create_with_seed(0, TEST_SEED, &out) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(out == NULL);
    CHECK(hash_table_create_with_seed(1, NULL, &out) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_create(0, &out) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_create(1, NULL) == HASH_TABLE_ERR_INVALID_ARGUMENT);
    CHECK(hash_table_create_with_seed((size_t)-1, TEST_SEED, &out) == HASH_TABLE_ERR_OVERFLOW);
    CHECK(out == NULL);
    hash_table_destroy(table);
}

static void test_destroy_releases_all_memory(void)
{
    const long live_before = test_allocator_live_count();
    hash_table_t *table = create_seeded_table(2);
    char key[8];

    for (int i = 0; i < 6; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        CHECK(set_text(table, key, "value") == HASH_TABLE_OK);
    }
    CHECK(set_text(table, "k1", "replaced") == HASH_TABLE_OK);
    CHECK(hash_table_delete(table, "k2", 2) == HASH_TABLE_OK);
    hash_table_destroy(table);
    CHECK(test_allocator_live_count() == live_before);
}

/* 확장이 필요 없는 신규 삽입은 엔트리, 키, 값 순으로 3번 할당한다. 어느 단계에서 실패해도 기존 상태와 메모리가 그대로여야 한다. */
static void test_allocation_failure_preserves_table(void)
{
    hash_table_t *table = create_seeded_table(4);

    CHECK(set_text(table, "a", "1") == HASH_TABLE_OK);
    CHECK(set_text(table, "b", "2") == HASH_TABLE_OK);
    const long live_before = test_allocator_live_count();

    for (size_t nth = 1; nth <= 3; nth++) {
        test_allocator_fail_nth(nth);
        CHECK(set_text(table, "c", "3") == HASH_TABLE_ERR_NO_MEMORY);
        test_allocator_fail_nth(0);
        CHECK(test_allocator_live_count() == live_before);
        CHECK(hash_table_size(table) == 2);
        CHECK(hash_table_bucket_count(table) == 4);
        CHECK(!has_key(table, "c"));
    }

    test_allocator_fail_nth(1);
    CHECK(set_text(table, "a", "new") == HASH_TABLE_ERR_NO_MEMORY);
    test_allocator_fail_nth(0);
    CHECK(test_allocator_live_count() == live_before);
    CHECK(hash_table_size(table) == 2);
    CHECK(has_text_value(table, "a", "1"));
    CHECK(has_text_value(table, "b", "2"));
    hash_table_destroy(table);

    for (size_t nth = 1; nth <= 2; nth++) {
        const long live_before_create = test_allocator_live_count();
        hash_table_t *failed = (hash_table_t *)&failed;
        test_allocator_fail_nth(nth);
        CHECK(hash_table_create_with_seed(4, TEST_SEED, &failed) == HASH_TABLE_ERR_NO_MEMORY);
        test_allocator_fail_nth(0);
        CHECK(failed == NULL);
        CHECK(test_allocator_live_count() == live_before_create);
    }
}

/* size == bucket_count까지는 그대로이고, 다음 신규 삽입에서만 정확히 두 배가 된다. 교체는 확장하지 않는다. */
static void check_growth_from(size_t initial_bucket_count)
{
    hash_table_t *table = create_seeded_table(initial_bucket_count);
    hash_table_set_outcome outcome = HASH_TABLE_SET_INSERTED;
    /* size_t 최댓값까지 담을 수 있어야 -Wformat-truncation 경고가 나지 않는다. */
    char key[32];

    for (size_t i = 0; i < initial_bucket_count; i++) {
        snprintf(key, sizeof(key), "g%zu", i);
        CHECK(set_text(table, key, key) == HASH_TABLE_OK);
        CHECK(hash_table_bucket_count(table) == initial_bucket_count);
    }
    CHECK(hash_table_set(table, "g0", 2, "boundary", 8, &outcome) == HASH_TABLE_OK);
    CHECK(outcome == HASH_TABLE_SET_REPLACED);
    CHECK(hash_table_size(table) == initial_bucket_count);
    CHECK(hash_table_bucket_count(table) == initial_bucket_count);

    CHECK(hash_table_set(table, "next", 4, "v", 1, &outcome) == HASH_TABLE_OK);
    CHECK(outcome == HASH_TABLE_SET_INSERTED);
    CHECK(hash_table_size(table) == initial_bucket_count + 1);
    CHECK(hash_table_bucket_count(table) == initial_bucket_count * 2);
    CHECK(has_text_value(table, "g0", "boundary"));
    CHECK(has_text_value(table, "next", "v"));
    for (size_t i = 1; i < initial_bucket_count; i++) {
        snprintf(key, sizeof(key), "g%zu", i);
        CHECK(has_text_value(table, key, key));
    }
    CHECK(has_consistent_layout(table));

    /* 삭제는 축소하지 않는다. */
    for (size_t i = 0; i < initial_bucket_count; i++) {
        snprintf(key, sizeof(key), "g%zu", i);
        CHECK(hash_table_delete(table, key, strlen(key)) == HASH_TABLE_OK);
    }
    CHECK(hash_table_size(table) == 1);
    CHECK(hash_table_bucket_count(table) == initial_bucket_count * 2);
    hash_table_destroy(table);
}

static void test_growth_boundary_doubles_exactly(void)
{
    const size_t initial_counts[] = {1, 3, 4, 5};

    for (size_t i = 0; i < sizeof(initial_counts) / sizeof(initial_counts[0]); i++) {
        check_growth_from(initial_counts[i]);
    }
}

/* 초기 버킷 1에서 여러 번 확장해도 빈 키, 빈 값, 이진 데이터를 포함한 모든 엔트리가 남는다. */
static void test_many_growths_keep_all_entries(void)
{
    hash_table_t *table = create_seeded_table(1);
    const uint8_t binary_key[3] = {0x00, 0xff, 0x00};
    const uint8_t binary_value[2] = {0x00, 0x7f};
    const void *value = NULL;
    size_t value_len = 99;
    char key[TEST_KEY_SIZE];
    size_t expected_bucket_count = 1;

    CHECK(hash_table_set(table, NULL, 0, NULL, 0, NULL) == HASH_TABLE_OK);
    CHECK(hash_table_set(table, binary_key, 3, binary_value, 2, NULL) == HASH_TABLE_OK);
    CHECK(hash_table_bucket_count(table) == 2);
    expected_bucket_count = 2;
    for (int i = 0; i < 58; i++) {
        snprintf(key, sizeof(key), "m%d", i);
        CHECK(set_text(table, key, key) == HASH_TABLE_OK);
        if (hash_table_size(table) > expected_bucket_count) {
            expected_bucket_count *= 2;
        }
        CHECK(hash_table_bucket_count(table) == expected_bucket_count);
    }
    CHECK(hash_table_size(table) == 60);
    CHECK(hash_table_bucket_count(table) == 64);
    CHECK(has_consistent_layout(table));

    CHECK(hash_table_get(table, "", 0, &value, &value_len) == HASH_TABLE_OK);
    CHECK(value != NULL && value_len == 0);
    CHECK(hash_table_get(table, binary_key, 3, &value, &value_len) == HASH_TABLE_OK);
    CHECK(value_len == 2 && memcmp(value, binary_value, 2) == 0);
    for (int i = 0; i < 58; i++) {
        snprintf(key, sizeof(key), "m%d", i);
        CHECK(has_text_value(table, key, key));
    }

    /* 확장 이후의 교체, 삭제, 재삽입. */
    for (int i = 0; i < 58; i += 3) {
        snprintf(key, sizeof(key), "m%d", i);
        CHECK(set_text(table, key, "replaced") == HASH_TABLE_OK);
    }
    for (int i = 1; i < 58; i += 3) {
        snprintf(key, sizeof(key), "m%d", i);
        CHECK(hash_table_delete(table, key, strlen(key)) == HASH_TABLE_OK);
    }
    CHECK(set_text(table, "m1", "again") == HASH_TABLE_OK);
    CHECK(hash_table_bucket_count(table) == 64);
    for (int i = 0; i < 58; i++) {
        snprintf(key, sizeof(key), "m%d", i);
        if (i == 1) {
            CHECK(has_text_value(table, key, "again"));
        } else if (i % 3 == 0) {
            CHECK(has_text_value(table, key, "replaced"));
        } else if (i % 3 == 1) {
            CHECK(!has_key(table, key));
        } else {
            CHECK(has_text_value(table, key, key));
        }
    }
    CHECK(has_consistent_layout(table));
    hash_table_destroy(table);
}

/* 한 체인에 몰린 키가 확장 후 새 버킷 수 기준으로 다시 나뉘고, 잃거나 중복되지 않는다. */
static void test_growth_redistributes_colliding_chain(void)
{
    char keys[8][TEST_KEY_SIZE];
    CHECK(find_colliding_keys(8, keys, 8) == 8);
    hash_table_t *table = create_seeded_table(8);

    for (int i = 0; i < 8; i++) {
        CHECK(set_text(table, keys[i], keys[i]) == HASH_TABLE_OK);
    }
    CHECK(hash_table_bucket_count(table) == 8);
    hash_table_test_link links[MAX_TEST_LINKS];
    CHECK(hash_table_test_collect_links(table, links, MAX_TEST_LINKS) == 8);
    CHECK(links[0].bucket_index == links[7].bucket_index);

    CHECK(set_text(table, "trigger", "t") == HASH_TABLE_OK);
    CHECK(hash_table_bucket_count(table) == 16);
    CHECK(hash_table_size(table) == 9);
    CHECK(has_consistent_layout(table));
    for (int i = 0; i < 8; i++) {
        CHECK(has_text_value(table, keys[i], keys[i]));
    }

    /* 같은 체인이던 키가 hash % 16에서 갈라지는 경우가 있어야 재분산을 실제로 확인한 것이다. */
    int split = 0;
    for (int i = 1; i < 8; i++) {
        if (expected_bucket_index(keys[i], strlen(keys[i]), 16) != expected_bucket_index(keys[0], strlen(keys[0]), 16)) {
            split = 1;
        }
    }
    CHECK(split);
    hash_table_destroy(table);
}

/*
 * 확장이 필요한 신규 삽입은 엔트리, 키, 값, 새 버킷 배열 순으로 4번 할당한다.
 * 어느 지점에서 실패해도 기존 엔트리 주소와 next 연결, 개수, 버킷 수, outcome이 그대로여야 한다.
 */
static void test_growth_allocation_failure_preserves_table(void)
{
    char keys[4][TEST_KEY_SIZE];
    CHECK(find_colliding_keys(4, keys, 2) == 2);
    snprintf(keys[2], TEST_KEY_SIZE, "x2");
    snprintf(keys[3], TEST_KEY_SIZE, "x3");
    hash_table_t *table = create_seeded_table(4);

    for (int i = 0; i < 4; i++) {
        CHECK(set_text(table, keys[i], keys[i]) == HASH_TABLE_OK);
    }
    CHECK(hash_table_bucket_count(table) == 4);
    hash_table_test_link before[MAX_TEST_LINKS];
    hash_table_test_link after[MAX_TEST_LINKS];
    CHECK(hash_table_test_collect_links(table, before, MAX_TEST_LINKS) == 4);
    const long live_before = test_allocator_live_count();

    for (size_t nth = 1; nth <= 4; nth++) {
        hash_table_set_outcome outcome = (hash_table_set_outcome)99;
        test_allocator_fail_nth(nth);
        CHECK(hash_table_set(table, "fresh", 5, "v", 1, &outcome) == HASH_TABLE_ERR_NO_MEMORY);
        test_allocator_fail_nth(0);
        CHECK(outcome == (hash_table_set_outcome)99);
        CHECK(test_allocator_live_count() == live_before);
        CHECK(hash_table_size(table) == 4);
        CHECK(hash_table_bucket_count(table) == 4);
        CHECK(!has_key(table, "fresh"));
        CHECK(hash_table_test_collect_links(table, after, MAX_TEST_LINKS) == 4);
        CHECK(same_links(before, after, 4));
        for (int i = 0; i < 4; i++) {
            CHECK(has_text_value(table, keys[i], keys[i]));
        }
    }

    /* 경계에서 교체가 실패해도 값과 용량이 그대로다. */
    test_allocator_fail_nth(1);
    CHECK(set_text(table, keys[0], "new") == HASH_TABLE_ERR_NO_MEMORY);
    test_allocator_fail_nth(0);
    CHECK(has_text_value(table, keys[0], keys[0]));
    CHECK(hash_table_bucket_count(table) == 4);
    CHECK(test_allocator_live_count() == live_before);

    /* 5번째 할당까지 실패가 닿지 않으면 확장 삽입이 정확히 4번 할당으로 끝났다는 뜻이다. */
    hash_table_set_outcome outcome = HASH_TABLE_SET_REPLACED;
    test_allocator_fail_nth(5);
    CHECK(hash_table_set(table, "fresh", 5, "v", 1, &outcome) == HASH_TABLE_OK);
    test_allocator_fail_nth(0);
    CHECK(outcome == HASH_TABLE_SET_INSERTED);
    CHECK(hash_table_bucket_count(table) == 8);
    CHECK(hash_table_size(table) == 5);
    /* 엔트리 3개와 새 배열 1개가 늘고 옛 배열 1개가 해제된다. */
    CHECK(test_allocator_live_count() == live_before + 3);
    CHECK(has_text_value(table, "fresh", "v"));
    for (int i = 0; i < 4; i++) {
        CHECK(has_text_value(table, keys[i], keys[i]));
    }
    CHECK(has_consistent_layout(table));
    hash_table_destroy(table);
}

/* 입력 키/값이 테이블이 빌려준 버퍼를 가리켜도, 교체와 확장 삽입 모두 복사를 먼저 끝내므로 안전하다. */
static void test_borrowed_input_survives_replace_and_growth(void)
{
    hash_table_t *table = create_seeded_table(2);
    const void *borrowed = NULL;
    size_t borrowed_len = 0;

    CHECK(set_text(table, "a", "alias-key") == HASH_TABLE_OK);
    CHECK(set_text(table, "b", "b") == HASH_TABLE_OK);
    CHECK(hash_table_get(table, "a", 1, &borrowed, &borrowed_len) == HASH_TABLE_OK);
    CHECK(hash_table_set(table, "a", 1, borrowed, borrowed_len, NULL) == HASH_TABLE_OK);
    CHECK(has_text_value(table, "a", "alias-key"));
    CHECK(hash_table_bucket_count(table) == 2);

    CHECK(hash_table_get(table, "a", 1, &borrowed, &borrowed_len) == HASH_TABLE_OK);
    CHECK(hash_table_set(table, borrowed, borrowed_len, borrowed, borrowed_len, NULL) == HASH_TABLE_OK);
    CHECK(hash_table_bucket_count(table) == 4);
    CHECK(has_text_value(table, "alias-key", "alias-key"));
    CHECK(has_text_value(table, "a", "alias-key"));
    CHECK(has_text_value(table, "b", "b"));
    hash_table_destroy(table);
}

/* 실제 거대 할당 없이 개수, 두 배 용량, 배열 바이트 크기의 넘침 경계를 확인한다. */
static void test_plan_bucket_count_overflow_boundaries(void)
{
    const size_t max_array_count = SIZE_MAX / sizeof(void *);
    size_t planned = 777;

    CHECK(hash_table_test_plan_bucket_count(3, 4, &planned) == HASH_TABLE_OK && planned == 4);
    CHECK(hash_table_test_plan_bucket_count(4, 4, &planned) == HASH_TABLE_OK && planned == 8);
    CHECK(hash_table_test_plan_bucket_count(3, 3, &planned) == HASH_TABLE_OK && planned == 6);
    CHECK(hash_table_test_plan_bucket_count(0, 1, &planned) == HASH_TABLE_OK && planned == 1);

    planned = 777;
    CHECK(hash_table_test_plan_bucket_count(SIZE_MAX, SIZE_MAX, &planned) == HASH_TABLE_ERR_OVERFLOW);
    CHECK(hash_table_test_plan_bucket_count(SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1, &planned) == HASH_TABLE_ERR_OVERFLOW);
    CHECK(hash_table_test_plan_bucket_count(max_array_count / 2 + 1, max_array_count / 2 + 1, &planned)
          == HASH_TABLE_ERR_OVERFLOW);
    CHECK(planned == 777);
    CHECK(hash_table_test_plan_bucket_count(max_array_count / 2, max_array_count / 2, &planned) == HASH_TABLE_OK);
    CHECK(planned == (max_array_count / 2) * 2);
    /* 확장이 필요 없으면 큰 버킷 수도 그대로 둔다. */
    CHECK(hash_table_test_plan_bucket_count(0, SIZE_MAX, &planned) == HASH_TABLE_OK && planned == SIZE_MAX);
}

typedef struct {
    const char *name;
    void (*run)(void);
} test_case;

int main(void)
{
    const test_case tests[] = {
        {"siphash_matches_official_vectors", test_siphash_matches_official_vectors},
        {"create_and_destroy_empty", test_create_and_destroy_empty},
        {"set_then_get", test_set_then_get},
        {"missing_key_get_and_delete", test_missing_key_get_and_delete},
        {"replace_changes_value_and_keeps_size", test_replace_changes_value_and_keeps_size},
        {"distinct_keys_are_independent", test_distinct_keys_are_independent},
        {"colliding_chain_operations", test_colliding_chain_operations},
        {"delete_head_middle_tail", test_delete_head_middle_tail},
        {"reinsert_after_deleting_last_entry", test_reinsert_after_deleting_last_entry},
        {"table_owns_copies_of_caller_buffers", test_table_owns_copies_of_caller_buffers},
        {"empty_binary_and_invalid_arguments", test_empty_binary_and_invalid_arguments},
        {"destroy_releases_all_memory", test_destroy_releases_all_memory},
        {"allocation_failure_preserves_table", test_allocation_failure_preserves_table},
        {"growth_boundary_doubles_exactly", test_growth_boundary_doubles_exactly},
        {"many_growths_keep_all_entries", test_many_growths_keep_all_entries},
        {"growth_redistributes_colliding_chain", test_growth_redistributes_colliding_chain},
        {"growth_allocation_failure_preserves_table", test_growth_allocation_failure_preserves_table},
        {"borrowed_input_survives_replace_and_growth", test_borrowed_input_survives_replace_and_growth},
        {"plan_bucket_count_overflow_boundaries", test_plan_bucket_count_overflow_boundaries},
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    for (size_t i = 0; i < test_count; i++) {
        const long live_before = test_allocator_live_count();
        const long errors_before = test_allocator_error_count();
        current_test_failed = 0;
        tests[i].run();
        if (test_allocator_live_count() != live_before) {
            fprintf(stderr, "  leaked %ld allocation(s)\n", test_allocator_live_count() - live_before);
            current_test_failed = 1;
        }
        if (test_allocator_error_count() != errors_before) {
            fprintf(stderr, "  %ld invalid free/tracking error(s)\n", test_allocator_error_count() - errors_before);
            current_test_failed = 1;
        }
        printf("[%s] %s\n", current_test_failed ? "FAIL" : "PASS", tests[i].name);
        if (!current_test_failed) {
            passed++;
        }
    }
    printf("%zu/%zu tests passed\n", passed, test_count);

    /* 테스트별 검사와 별개로, 전체 실행 후 테이블 할당이 하나도 남지 않았는지 확인한다. */
    const int all_freed = test_allocator_live_count() == 0 && test_allocator_error_count() == 0;
    printf("allocator: %ld live, %ld error(s)\n", test_allocator_live_count(), test_allocator_error_count());
    return passed == test_count && all_freed ? EXIT_SUCCESS : EXIT_FAILURE;
}
