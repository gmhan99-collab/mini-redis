#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "store.h"
#include "test_allocator.h"

static int current_test_failed = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
            current_test_failed = 1;                                         \
        }                                                                    \
    } while (0)

static store_t *create_store(void)
{
    store_t *store = NULL;
    CHECK(store_create(8, &store) == STORE_OK);
    return store;
}

static store_status put_text(store_t *store, const char *key, const char *value,
                             uint64_t ttl_ms, uint64_t now_ms)
{
    return store_put(store, key, strlen(key), value, strlen(value), ttl_ms, now_ms);
}

static store_status get_status(store_t *store, const char *key, uint64_t now_ms)
{
    const void *value = NULL;
    size_t value_len = 0;
    return store_get(store, key, strlen(key), now_ms, &value, &value_len);
}

static int has_text_value(store_t *store, const char *key, const char *expected, uint64_t now_ms)
{
    const void *value = NULL;
    size_t value_len = 0;
    if (store_get(store, key, strlen(key), now_ms, &value, &value_len) != STORE_OK) {
        return 0;
    }
    return value_len == strlen(expected) && memcmp(value, expected, value_len) == 0;
}

static void test_no_ttl_never_expires(void)
{
    store_t *store = create_store();
    CHECK(put_text(store, "k", "v", 0, 100) == STORE_OK);
    CHECK(has_text_value(store, "k", "v", UINT64_MAX));
    store_destroy(store);
}

static void test_expiry_boundary(void)
{
    store_t *store = create_store();
    CHECK(put_text(store, "k", "v", 100, 1000) == STORE_OK);
    CHECK(has_text_value(store, "k", "v", 1000));
    CHECK(has_text_value(store, "k", "v", 1099));
    CHECK(get_status(store, "k", 1100) == STORE_EXPIRED);
    CHECK(get_status(store, "k", 1100) == STORE_NOT_FOUND);
    store_destroy(store);
}

static void test_delete_reports_expired_then_not_found(void)
{
    store_t *store = create_store();
    CHECK(put_text(store, "k", "v", 10, 0) == STORE_OK);
    CHECK(store_delete(store, "k", 1, 10) == STORE_EXPIRED);
    CHECK(store_delete(store, "k", 1, 10) == STORE_NOT_FOUND);
    CHECK(get_status(store, "k", 10) == STORE_NOT_FOUND);

    CHECK(put_text(store, "live", "v", 10, 0) == STORE_OK);
    CHECK(store_delete(store, "live", 4, 9) == STORE_OK);
    CHECK(get_status(store, "live", 9) == STORE_NOT_FOUND);
    store_destroy(store);
}

static void test_overwrite_replaces_or_removes_ttl(void)
{
    store_t *store = create_store();
    CHECK(put_text(store, "k", "a", 10, 0) == STORE_OK);
    CHECK(put_text(store, "k", "b", 100, 5) == STORE_OK);
    CHECK(has_text_value(store, "k", "b", 50));
    CHECK(get_status(store, "k", 105) == STORE_EXPIRED);

    CHECK(put_text(store, "k", "c", 10, 0) == STORE_OK);
    CHECK(put_text(store, "k", "d", 0, 5) == STORE_OK);
    CHECK(has_text_value(store, "k", "d", 1000000));
    store_destroy(store);
}

static void test_put_over_expired_key_stores_new_value(void)
{
    store_t *store = create_store();
    CHECK(put_text(store, "k", "old", 10, 0) == STORE_OK);
    CHECK(put_text(store, "k", "new", 10, 20) == STORE_OK);
    CHECK(has_text_value(store, "k", "new", 29));
    store_destroy(store);
}

static void test_binary_and_empty_values(void)
{
    store_t *store = create_store();
    const unsigned char key[] = { 'a', 0, 'b' };
    const unsigned char value[] = { 0, 0xFF, 0, 7 };
    const void *out = NULL;
    size_t out_len = 99;

    CHECK(store_put(store, key, sizeof(key), value, sizeof(value), 0, 0) == STORE_OK);
    CHECK(store_get(store, key, sizeof(key), 0, &out, &out_len) == STORE_OK);
    CHECK(out_len == sizeof(value) && memcmp(out, value, sizeof(value)) == 0);
    CHECK(store_get(store, key, 1, 0, &out, &out_len) == STORE_NOT_FOUND);

    CHECK(store_put(store, NULL, 0, NULL, 0, 0, 0) == STORE_OK);
    out = NULL;
    CHECK(store_get(store, NULL, 0, 0, &out, &out_len) == STORE_OK);
    CHECK(out != NULL && out_len == 0);
    store_destroy(store);
}

static void test_invalid_arguments_and_overflow_preserve_data(void)
{
    store_t *store = create_store();
    const void *out = NULL;
    size_t out_len = 0;

    CHECK(store_create(8, NULL) == STORE_INVALID_ARGUMENT);
    store_t *bad = (store_t *)&out;
    CHECK(store_create(0, &bad) == STORE_INVALID_ARGUMENT);
    CHECK(bad == NULL);

    CHECK(put_text(store, "k", "v", 50, 0) == STORE_OK);
    CHECK(store_put(NULL, "k", 1, "x", 1, 0, 0) == STORE_INVALID_ARGUMENT);
    CHECK(store_put(store, NULL, 1, "x", 1, 0, 0) == STORE_INVALID_ARGUMENT);
    CHECK(store_put(store, "k", 1, NULL, 1, 0, 0) == STORE_INVALID_ARGUMENT);
    CHECK(store_put(store, "k", 1, "x", 1, 1, UINT64_MAX) == STORE_OVERFLOW);
    CHECK(store_put(store, "k", 1, "x", 1, UINT64_MAX, 1) == STORE_OVERFLOW);
    CHECK(store_put(store, "k", 1, "x", SIZE_MAX, 0, 0) == STORE_OVERFLOW);
    CHECK(store_put(store, "k", 1, "x", 1, UINT64_MAX - 5, 5) == STORE_OK);
    CHECK(has_text_value(store, "k", "x", UINT64_MAX - 1));
    CHECK(get_status(store, "k", UINT64_MAX) == STORE_EXPIRED);

    CHECK(put_text(store, "k", "v", 50, 0) == STORE_OK);
    CHECK(store_get(NULL, "k", 1, 0, &out, &out_len) == STORE_INVALID_ARGUMENT);
    CHECK(store_get(store, NULL, 1, 0, &out, &out_len) == STORE_INVALID_ARGUMENT);
    CHECK(store_get(store, "k", 1, 0, NULL, &out_len) == STORE_INVALID_ARGUMENT);
    CHECK(store_get(store, "k", 1, 0, &out, NULL) == STORE_INVALID_ARGUMENT);
    CHECK(store_delete(NULL, "k", 1, 0) == STORE_INVALID_ARGUMENT);
    CHECK(store_delete(store, NULL, 1, 0) == STORE_INVALID_ARGUMENT);
    CHECK(has_text_value(store, "k", "v", 49));
    store_destroy(store);
}

/* store_put은 레코드 버퍼 할당 후 hash_table_set이 내부 할당을 한다. 각 단계 실패를 주입한다. */
static void test_allocation_failure_preserves_value_and_ttl(void)
{
    store_t *store = create_store();
    CHECK(put_text(store, "k", "old", 100, 0) == STORE_OK);
    const long live_before = test_allocator_live_count();

    for (size_t nth = 1; nth <= 2; nth++) {
        test_allocator_fail_nth(nth);
        CHECK(put_text(store, "k", "new", 0, 10) == STORE_NO_MEMORY);
        test_allocator_fail_nth(0);
        CHECK(test_allocator_live_count() == live_before);
        CHECK(has_text_value(store, "k", "old", 99));
    }
    CHECK(get_status(store, "k", 100) == STORE_EXPIRED);

    test_allocator_fail_nth(1);
    CHECK(put_text(store, "fresh", "v", 0, 0) == STORE_NO_MEMORY);
    test_allocator_fail_nth(0);
    CHECK(get_status(store, "fresh", 0) == STORE_NOT_FOUND);
    store_destroy(store);
}

/*
 * 초기 버킷 1인 store에 키 24개를 넣으면 해시 테이블이 1 -> 32로 여러 번 확장된다.
 * 확장 후에도 각 키의 값과 만료 시각이 같은 레코드로 함께 옮겨져야 한다.
 */
static void test_growth_keeps_values_and_ttl_paired(void)
{
    store_t *store = NULL;
    char key[16];
    char value[16];

    CHECK(store_create(1, &store) == STORE_OK);
    for (int i = 0; i < 24; i++) {
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(value, sizeof(value), "value%d", i);
        const uint64_t ttl_ms = i % 2 == 0 ? 0 : (uint64_t)(100 + i);
        CHECK(put_text(store, key, value, ttl_ms, 1000) == STORE_OK);
    }
    for (int i = 0; i < 24; i++) {
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(value, sizeof(value), "value%d", i);
        CHECK(has_text_value(store, key, value, 1099));
    }
    /* 홀수 키 i는 1100 + i에 만료된다. 직전에는 살아 있고, 그 시각에 expired 후 not_found가 된다. */
    for (int i = 1; i < 24; i += 2) {
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(value, sizeof(value), "value%d", i);
        CHECK(has_text_value(store, key, value, (uint64_t)(1099 + i)));
        CHECK(get_status(store, key, (uint64_t)(1100 + i)) == STORE_EXPIRED);
        CHECK(get_status(store, key, (uint64_t)(1100 + i)) == STORE_NOT_FOUND);
    }
    for (int i = 0; i < 24; i += 2) {
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(value, sizeof(value), "value%d", i);
        CHECK(has_text_value(store, key, value, UINT64_MAX));
    }
    /* 만료로 지운 뒤 재삽입과 교체도 확장된 테이블에서 정상 동작한다. */
    CHECK(put_text(store, "key1", "again", 10, 5000) == STORE_OK);
    CHECK(put_text(store, "key0", "replaced", 0, 5000) == STORE_OK);
    CHECK(has_text_value(store, "key1", "again", 5009));
    CHECK(get_status(store, "key1", 5010) == STORE_EXPIRED);
    CHECK(has_text_value(store, "key0", "replaced", 5010));
    store_destroy(store);
}

static void test_create_allocation_failure(void)
{
    const long live_before = test_allocator_live_count();
    for (size_t nth = 1; nth <= 2; nth++) {
        store_t *store = (store_t *)&nth;
        test_allocator_fail_nth(nth);
        CHECK(store_create(8, &store) == STORE_NO_MEMORY);
        test_allocator_fail_nth(0);
        CHECK(store == NULL);
        CHECK(test_allocator_live_count() == live_before);
    }
}

static void test_status_messages(void)
{
    CHECK(strcmp(store_status_message(STORE_EXPIRED), "expired") == 0);
    CHECK(strcmp(store_status_message(STORE_NOT_FOUND), "not_found") == 0);
    CHECK(strcmp(store_status_message((store_status)99), "unknown") == 0);
}

typedef struct {
    const char *name;
    void (*run)(void);
} test_case;

int main(void)
{
    const test_case tests[] = {
        { "no_ttl_never_expires", test_no_ttl_never_expires },
        { "expiry_boundary", test_expiry_boundary },
        { "delete_reports_expired_then_not_found", test_delete_reports_expired_then_not_found },
        { "overwrite_replaces_or_removes_ttl", test_overwrite_replaces_or_removes_ttl },
        { "put_over_expired_key_stores_new_value", test_put_over_expired_key_stores_new_value },
        { "binary_and_empty_values", test_binary_and_empty_values },
        { "invalid_arguments_and_overflow_preserve_data", test_invalid_arguments_and_overflow_preserve_data },
        { "allocation_failure_preserves_value_and_ttl", test_allocation_failure_preserves_value_and_ttl },
        { "growth_keeps_values_and_ttl_paired", test_growth_keeps_values_and_ttl_paired },
        { "create_allocation_failure", test_create_allocation_failure },
        { "status_messages", test_status_messages },
    };
    const size_t test_count = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    for (size_t i = 0; i < test_count; i++) {
        current_test_failed = 0;
        tests[i].run();
        if (current_test_failed) {
            printf("FAIL %s\n", tests[i].name);
        } else {
            printf("PASS %s\n", tests[i].name);
            passed++;
        }
    }
    const int leak_free = test_allocator_live_count() == 0 && test_allocator_error_count() == 0;
    if (!leak_free) {
        printf("FAIL allocator: live=%ld errors=%ld\n", test_allocator_live_count(), test_allocator_error_count());
    }
    printf("%zu/%zu tests passed\n", passed, test_count);
    return passed == test_count && leak_free ? 0 : 1;
}
