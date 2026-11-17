#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "hash_table.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "measurement_clock.h"

#include "key_hash.h"
#include "random_seed.h"

#ifdef HASH_TABLE_TEST_ALLOCATOR
#include "hash_table_test_access.h"
#endif

/* 단위 테스트 빌드에서만 할당 실패 주입과 누수 추적용 할당기로 바꾼다. */
#ifdef HASH_TABLE_TEST_ALLOCATOR
void *test_allocator_malloc(size_t size);
void test_allocator_free(void *ptr);
#define HASH_TABLE_MALLOC(size) test_allocator_malloc(size)
#define HASH_TABLE_FREE(ptr) test_allocator_free(ptr)
#else
#define HASH_TABLE_MALLOC(size) malloc(size)
#define HASH_TABLE_FREE(ptr) free(ptr)
#endif

/* Private test seam: production always uses the existing monotonic clock. */
#ifdef HASH_TABLE_TEST_CLOCK
#ifndef HASH_TABLE_TEST_ALLOCATOR
#error HASH_TABLE_TEST_CLOCK requires the test-only allocator build
#endif
uint64_t hash_table_test_clock_ticks(void);
double hash_table_test_clock_ms(uint64_t start, uint64_t end);
#define REHASH_TICKS() hash_table_test_clock_ticks()
#define REHASH_MS(start, end) hash_table_test_clock_ms(start, end)
#else
#define REHASH_TICKS() measurement_ticks()
#define REHASH_MS(start, end) measurement_ms(start, end)
#endif

typedef struct hash_table_entry {
    uint8_t *key;
    size_t key_len;
    uint8_t *value;
    size_t value_len;
    struct hash_table_entry *next;
    struct hash_table_entry *left, *right, *parent;
    uint64_t hash;
    int red;
} hash_table_entry;

struct hash_table {
    hash_table_entry **buckets;
    size_t bucket_count;
    size_t size;
    size_t initial_buckets;
    hash_table_bucket_structure structure;
    int explicit_seed;
    uint64_t rehash_count;
    double rehash_ms;
#ifdef HASH_TABLE_TEST_ALLOCATOR
    int test_constant_hash;
#endif
    uint8_t seed[HASH_TABLE_SEED_SIZE];
};

static int is_valid_bytes(const void *data, size_t data_len)
{
    return data != NULL || data_len == 0;
}

/* 길이 0도 1바이트를 할당해, 저장된 키/값 포인터가 항상 NULL이 아니도록 한다. */
static uint8_t *copy_bytes(const void *data, size_t data_len)
{
    uint8_t *copy = HASH_TABLE_MALLOC(data_len == 0 ? 1 : data_len);

    if (copy == NULL) {
        return NULL;
    }
    if (data_len > 0) {
        memcpy(copy, data, data_len);
    }
    return copy;
}

#include "bucket_tree.h"

static uint64_t table_key_hash(const hash_table_t *table, const void *key, size_t key_len)
{
#ifdef HASH_TABLE_TEST_ALLOCATOR
    if (table->test_constant_hash) {
        return 0;
    }
#endif
    return key_hash_siphash24(table->seed, key, key_len);
}

static int entry_has_key(const hash_table_entry *entry, const void *key, size_t key_len)
{
    if (entry->key_len != key_len) {
        return 0;
    }
    return key_len == 0 || memcmp(entry->key, key, key_len) == 0;
}

/*
 * 체인에서 키를 찾아, 그 엔트리를 가리키는 링크(버킷 슬롯 또는 이전 엔트리의 next)의 주소를 돌려준다.
 * 키가 없으면 체인 끝의 NULL 링크 주소를 돌려준다. 링크 주소를 쓰면 머리/중간/꼬리 삭제를 같은 방식으로 처리할 수 있다.
 */
static hash_table_entry **find_entry_link(hash_table_entry **bucket, const void *key, size_t key_len)
{
    hash_table_entry **link = bucket;

    while (*link != NULL && !entry_has_key(*link, key, key_len)) {
        link = &(*link)->next;
    }
    return link;
}

static void destroy_entry(hash_table_entry *entry)
{
    HASH_TABLE_FREE(entry->key);
    HASH_TABLE_FREE(entry->value);
    HASH_TABLE_FREE(entry);
}

static hash_table_entry *create_entry(const void *key, size_t key_len, const void *value, size_t value_len)
{
    hash_table_entry *entry = HASH_TABLE_MALLOC(sizeof(*entry));

    if (entry == NULL) {
        return NULL;
    }
    entry->key = copy_bytes(key, key_len);
    entry->value = copy_bytes(value, value_len);
    if (entry->key == NULL || entry->value == NULL) {
        destroy_entry(entry);
        return NULL;
    }
    entry->key_len = key_len;
    entry->value_len = value_len;
    entry->next = NULL;
    entry->left = entry->right = entry->parent = NULL;
    entry->red = 0;
    entry->hash = 0;
    return entry;
}

/* 새 값의 할당이 끝난 뒤에만 기존 값을 해제해, 실패 시 기존 값이 남도록 한다. */
static hash_table_status replace_entry_value(hash_table_entry *entry, const void *value, size_t value_len)
{
    uint8_t *new_value = copy_bytes(value, value_len);

    if (new_value == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    HASH_TABLE_FREE(entry->value);
    entry->value = new_value;
    entry->value_len = value_len;
    return HASH_TABLE_OK;
}

static int is_bucket_array_size_valid(size_t bucket_count)
{
    return bucket_count <= SIZE_MAX / sizeof(hash_table_entry *);
}

/* 호출자가 is_bucket_array_size_valid로 바이트 크기 넘침을 먼저 확인해야 한다. */
static hash_table_entry **allocate_empty_buckets(size_t bucket_count)
{
    hash_table_entry **buckets = HASH_TABLE_MALLOC(bucket_count * sizeof(*buckets));

    if (buckets == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < bucket_count; i++) {
        buckets[i] = NULL;
    }
    return buckets;
}

/*
 * 신규 키 하나를 넣은 뒤의 버킷 수를 정한다. size + 1이 버킷 수를 넘으면 정확히 두 배로 늘린다.
 * 부하율 1은 이번 단계의 정책일 뿐 성능 최적값이 아니다.
 * 개수, 두 배 용량, 배열 바이트 크기의 넘침을 할당 전에 모두 검사해 테이블을 건드리기 전에 실패한다.
 */
static hash_table_status plan_bucket_count_for_insert(size_t size, size_t bucket_count, size_t *out_bucket_count)
{
    if (size == SIZE_MAX) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    if (size + 1 <= bucket_count) {
        *out_bucket_count = bucket_count;
        return HASH_TABLE_OK;
    }
    if (bucket_count > SIZE_MAX / 2 || !is_bucket_array_size_valid(bucket_count * 2)) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    *out_bucket_count = bucket_count * 2;
    return HASH_TABLE_OK;
}

static hash_table_status allocate_table(size_t bucket_count, hash_table_t **out_table)
{
    if (!is_bucket_array_size_valid(bucket_count)) {
        return HASH_TABLE_ERR_OVERFLOW;
    }
    hash_table_t *table = HASH_TABLE_MALLOC(sizeof(*table));
    if (table == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    table->buckets = allocate_empty_buckets(bucket_count);
    if (table->buckets == NULL) {
        HASH_TABLE_FREE(table);
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    table->bucket_count = bucket_count;
    table->size = 0;
    table->initial_buckets = bucket_count;
    table->structure = HASH_TABLE_LIST;
    table->explicit_seed = 1;
    table->rehash_count = 0;
    table->rehash_ms = 0;
#ifdef HASH_TABLE_TEST_ALLOCATOR
    table->test_constant_hash = 0;
#endif
    *out_table = table;
    return HASH_TABLE_OK;
}

hash_table_status hash_table_create_config(const hash_table_config *config, hash_table_t **out_table)
{
    if (!out_table) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    *out_table = NULL;
    if (!config || !config->bucket_count ||
        (config->bucket_structure != HASH_TABLE_LIST && config->bucket_structure != HASH_TABLE_RBTREE)) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    uint8_t seed[HASH_TABLE_SEED_SIZE];
    if (config->seed) {
        memcpy(seed, config->seed, sizeof(seed));
    } else if (!random_seed_fill(seed, sizeof(seed))) {
        return HASH_TABLE_ERR_RANDOM_SOURCE;
    }
    hash_table_t *table = NULL;
    const hash_table_status status = allocate_table(config->bucket_count, &table);
    if (status != HASH_TABLE_OK) {
        return status;
    }
    memcpy(table->seed, seed, sizeof(seed));
    table->structure = config->bucket_structure;
    table->explicit_seed = config->seed != NULL;
    *out_table = table;
    return HASH_TABLE_OK;
}

hash_table_status hash_table_create_with_seed(size_t bucket_count, const uint8_t seed[HASH_TABLE_SEED_SIZE], hash_table_t **out_table)
{
    if (!seed) {
        if (out_table) {
            *out_table = NULL;
        }
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    const hash_table_config config = {bucket_count, HASH_TABLE_LIST, seed};
    return hash_table_create_config(&config, out_table);
}

hash_table_status hash_table_create(size_t bucket_count, hash_table_t **out_table)
{
    const hash_table_config config = {bucket_count, HASH_TABLE_LIST, NULL};
    return hash_table_create_config(&config, out_table);
}

static void destroy_tree(hash_table_entry *entry)
{
    if (entry) {
        destroy_tree(entry->left);
        destroy_tree(entry->right);
        destroy_entry(entry);
    }
}

void hash_table_destroy(hash_table_t *table)
{
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < table->bucket_count; i++) {
        if (table->structure == HASH_TABLE_RBTREE) {
            destroy_tree(table->buckets[i]);
            continue;
        }
        hash_table_entry *entry = table->buckets[i];
        while (entry != NULL) {
            hash_table_entry *next = entry->next;
            destroy_entry(entry);
            entry = next;
        }
    }
    HASH_TABLE_FREE(table->buckets);
    HASH_TABLE_FREE(table);
}

/* 체인 순서는 의미가 없으므로 O(1)인 머리 삽입을 쓴다. */
static void link_entry_at_head(hash_table_entry **bucket, hash_table_entry *entry)
{
    entry->next = *bucket;
    *bucket = entry;
}

/*
 * 엔트리를 복제하거나 다시 만들지 않고 next 연결만 바꿔 미리 할당한 새 배열로 옮긴 뒤 옛 배열을 해제한다.
 * 실패할 수 있는 작업이 없으므로, 시작한 뒤에는 항상 끝까지 진행된다.
 */
static void move_tree(hash_table_entry *entry, hash_table_entry **buckets, size_t count)
{
    if (!entry) {
        return;
    }
    hash_table_entry *left = entry->left;
    hash_table_entry *right = entry->right;
    tree_insert(&buckets[entry->hash % count], entry);
    move_tree(left, buckets, count);
    move_tree(right, buckets, count);
}

static void move_entries_to_buckets(hash_table_t *table, hash_table_entry **new_buckets, size_t new_bucket_count, uint64_t start)
{
    for (size_t i = 0; i < table->bucket_count; i++) {
        if (table->structure == HASH_TABLE_RBTREE) {
            move_tree(table->buckets[i], new_buckets, new_bucket_count);
        } else {
            hash_table_entry *entry = table->buckets[i];
            while (entry) {
                hash_table_entry *next = entry->next;
                link_entry_at_head(&new_buckets[entry->hash % new_bucket_count], entry);
                entry = next;
            }
        }
    }
    HASH_TABLE_FREE(table->buckets);
    table->buckets = new_buckets;
    table->bucket_count = new_bucket_count;
    table->rehash_count++;
    table->rehash_ms += REHASH_MS(start, REHASH_TICKS());
}

/*
 * 준비 단계(넘침 검사, 엔트리/키/값 복사, 필요 시 확장 배열 할당)를 모두 마친 뒤에만 체인을 바꾼다.
 * 준비가 실패하면 만든 자원만 해제하므로 테이블은 그대로다.
 * key와 value는 테이블이 빌려준 버퍼와 겹칠 수 있으므로, 체인을 바꾸기 전에 복사를 끝낸다.
 */
static hash_table_status insert_new_entry(hash_table_t *table,
                                          const void *key, size_t key_len,
                                          const void *value, size_t value_len, uint64_t hash)
{
    size_t target_bucket_count = 0;
    const hash_table_status plan_status =
        plan_bucket_count_for_insert(table->size, table->bucket_count, &target_bucket_count);
    if (plan_status != HASH_TABLE_OK) {
        return plan_status;
    }
    hash_table_entry *entry = create_entry(key, key_len, value, value_len);
    if (entry == NULL) {
        return HASH_TABLE_ERR_NO_MEMORY;
    }
    entry->hash = hash;
    if (target_bucket_count != table->bucket_count) {
        const uint64_t rehash_start = REHASH_TICKS();
        hash_table_entry **grown_buckets = allocate_empty_buckets(target_bucket_count);
        if (grown_buckets == NULL) {
            destroy_entry(entry);
            return HASH_TABLE_ERR_NO_MEMORY;
        }
        move_entries_to_buckets(table, grown_buckets, target_bucket_count, rehash_start);
    }
    /* 재배치로 옛 버킷 배열이 해제되었을 수 있으므로 현재 배열에서 위치를 다시 구한다. */
    const size_t index = (size_t)(entry->hash % table->bucket_count);
    if (table->structure == HASH_TABLE_RBTREE) {
        tree_insert(&table->buckets[index], entry);
    } else {
        link_entry_at_head(&table->buckets[index], entry);
    }
    table->size++;
    return HASH_TABLE_OK;
}

hash_table_status hash_table_set(hash_table_t *table,
                                 const void *key, size_t key_len,
                                 const void *value, size_t value_len,
                                 hash_table_set_outcome *outcome)
{
    if (table == NULL || !is_valid_bytes(key, key_len) || !is_valid_bytes(value, value_len)) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    const uint64_t hash = table_key_hash(table, key, key_len);
    hash_table_entry **bucket = &table->buckets[hash % table->bucket_count];
    hash_table_entry *existing = table->structure == HASH_TABLE_RBTREE
        ? tree_find(*bucket, hash, key, key_len)
        : *find_entry_link(bucket, key, key_len);

    /* 기존 키 교체는 개수가 늘지 않으므로 확장하지 않는다. */
    if (existing != NULL) {
        const hash_table_status status = replace_entry_value(existing, value, value_len);
        if (status == HASH_TABLE_OK && outcome != NULL) {
            *outcome = HASH_TABLE_SET_REPLACED;
        }
        return status;
    }
    const hash_table_status status = insert_new_entry(table, key, key_len, value, value_len, hash);
    if (status == HASH_TABLE_OK && outcome != NULL) {
        *outcome = HASH_TABLE_SET_INSERTED;
    }
    return status;
}

hash_table_status hash_table_get(const hash_table_t *table,
                                 const void *key, size_t key_len,
                                 const void **out_value, size_t *out_value_len)
{
    if (table == NULL || !is_valid_bytes(key, key_len) || out_value == NULL || out_value_len == NULL) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    const uint64_t hash = table_key_hash(table, key, key_len);
    hash_table_entry **bucket = &table->buckets[hash % table->bucket_count];
    const hash_table_entry *entry = table->structure == HASH_TABLE_RBTREE
        ? tree_find(*bucket, hash, key, key_len)
        : *find_entry_link(bucket, key, key_len);

    if (entry == NULL) {
        return HASH_TABLE_ERR_NOT_FOUND;
    }
    *out_value = entry->value;
    *out_value_len = entry->value_len;
    return HASH_TABLE_OK;
}

hash_table_status hash_table_delete(hash_table_t *table, const void *key, size_t key_len)
{
    if (table == NULL || !is_valid_bytes(key, key_len)) {
        return HASH_TABLE_ERR_INVALID_ARGUMENT;
    }
    const uint64_t hash = table_key_hash(table, key, key_len);
    hash_table_entry **bucket = &table->buckets[hash % table->bucket_count];
    hash_table_entry **link = NULL;
    hash_table_entry *entry;
    if (table->structure == HASH_TABLE_RBTREE) {
        entry = tree_find(*bucket, hash, key, key_len);
    } else {
        link = find_entry_link(bucket, key, key_len);
        entry = *link;
    }

    if (entry == NULL) {
        return HASH_TABLE_ERR_NOT_FOUND;
    }
    if (table->structure == HASH_TABLE_RBTREE) {
        tree_delete(bucket, entry);
    } else {
        *link = entry->next;
    }
    destroy_entry(entry);
    table->size--;
    return HASH_TABLE_OK;
}

size_t hash_table_size(const hash_table_t *table)
{
    return table == NULL ? 0 : table->size;
}

size_t hash_table_bucket_count(const hash_table_t *table)
{
    return table == NULL ? 0 : table->bucket_count;
}

const char *hash_table_status_message(hash_table_status status)
{
    switch (status) {
    case HASH_TABLE_OK:
        return "ok";
    case HASH_TABLE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case HASH_TABLE_ERR_NO_MEMORY:
        return "out of memory";
    case HASH_TABLE_ERR_NOT_FOUND:
        return "key not found";
    case HASH_TABLE_ERR_RANDOM_SOURCE:
        return "os random source failed";
    case HASH_TABLE_ERR_OVERFLOW:
        return "size overflow";
    }
    return "unknown status";
}

#ifdef HASH_TABLE_TEST_ALLOCATOR
/* 테스트 빌드 전용 관찰 함수. 선언과 설명은 hash_table_test_access.h에 있다. */
hash_table_status hash_table_test_plan_bucket_count(size_t size, size_t bucket_count, size_t *out_bucket_count)
{
    return plan_bucket_count_for_insert(size, bucket_count, out_bucket_count);
}

static void collect_test_tree(const hash_table_entry *entry, size_t bucket,
                               hash_table_test_link *out, size_t capacity, size_t *count)
{
    if (!entry) {
        return;
    }
    if (*count < capacity) {
        hash_table_test_link *link = &out[*count];
        memset(link, 0, sizeof(*link));
        link->bucket_index = bucket;
        link->entry = entry;
        link->key = entry->key;
        link->key_len = entry->key_len;
        link->left = entry->left;
        link->right = entry->right;
        link->parent = entry->parent;
        link->red = entry->red;
    }
    (*count)++;
    collect_test_tree(entry->left, bucket, out, capacity, count);
    collect_test_tree(entry->right, bucket, out, capacity, count);
}

size_t hash_table_test_collect_links(const hash_table_t *table, hash_table_test_link *out, size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < table->bucket_count; i++) {
        if (table->structure == HASH_TABLE_RBTREE) {
            collect_test_tree(table->buckets[i], i, out, capacity, &count);
            continue;
        }
        for (const hash_table_entry *entry = table->buckets[i]; entry; entry = entry->next) {
            if (count < capacity) {
                memset(&out[count], 0, sizeof(out[count]));
                out[count].bucket_index = i;
                out[count].entry = entry;
                out[count].next = entry->next;
                out[count].key = entry->key;
                out[count].key_len = entry->key_len;
            }
            count++;
        }
    }
    return count;
}
#endif

const char *hash_table_structure_name(hash_table_bucket_structure structure)
{
    return structure == HASH_TABLE_RBTREE ? "rbtree" : "list";
}

static size_t measure_tree(const hash_table_entry *node, size_t depth, size_t *height)
{
    if (!node) {
        return 0;
    }
    if (depth > *height) {
        *height = depth;
    }
    return 1 + measure_tree(node->left, depth + 1, height) + measure_tree(node->right, depth + 1, height);
}

int hash_table_snapshot(const hash_table_t *table, hash_table_stats *out, size_t *histogram, size_t histogram_len)
{
    if (!table || !out || (histogram_len && !histogram) || histogram_len > SIZE_MAX / sizeof(*histogram)) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (histogram_len) {
        memset(histogram, 0, histogram_len * sizeof(*histogram));
    }
    out->size = table->size;
    out->bucket_count = table->bucket_count;
    out->load_factor = (double)table->size / (double)table->bucket_count;
    out->rehash_count = table->rehash_count;
    out->rehash_ms = table->rehash_ms;
    int fits = 1;
    for (size_t i = 0; i < table->bucket_count; i++) {
        size_t count = 0;
        size_t height = 0;
        if (table->structure == HASH_TABLE_RBTREE) {
            count = measure_tree(table->buckets[i], 1, &height);
        } else {
            for (const hash_table_entry *entry = table->buckets[i]; entry; entry = entry->next) {
                count++;
            }
        }
        if (count) {
            out->nonempty_buckets++;
        }
        if (count > out->max_bucket_size) {
            out->max_bucket_size = count;
        }
        if (height > out->max_tree_height) {
            out->max_tree_height = height;
        }
        if (histogram) {
            if (count < histogram_len) {
                histogram[count]++;
            } else {
                fits = 0;
            }
        }
    }
    if (table->structure == HASH_TABLE_LIST) {
        out->max_chain_length = out->max_bucket_size;
    }
    return fits;
}

static int validate_tree(const hash_table_entry *node, const hash_table_entry *parent,
                         const hash_table_entry *lower, const hash_table_entry *upper,
                         size_t bucket, size_t buckets, size_t *count, size_t budget)
{
    if (!node) {
        return 1;
    }
    if (!budget || node->parent != parent || node->hash % buckets != bucket ||
        (lower && compare_key(node->hash, node->key, node->key_len, lower) <= 0) ||
        (upper && compare_key(node->hash, node->key, node->key_len, upper) >= 0) ||
        (node->red && (is_red(node->left) || is_red(node->right)))) {
        return 0;
    }
    (*count)++;
    const int left = validate_tree(node->left, node, lower, node, bucket, buckets, count, budget - 1);
    const int right = validate_tree(node->right, node, node, upper, bucket, buckets, count, budget - 1);
    if (!left || left != right) {
        return 0;
    }
    return left + !node->red;
}

int hash_table_validate(const hash_table_t *table)
{
    if (!table) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < table->bucket_count; i++) {
        if (table->structure == HASH_TABLE_RBTREE) {
            if (is_red(table->buckets[i]) || !validate_tree(table->buckets[i], NULL, NULL, NULL,
                                                          i, table->bucket_count, &count, table->size + 1)) {
                return 0;
            }
        } else {
            for (const hash_table_entry *node = table->buckets[i]; node; node = node->next) {
                if (++count > table->size || node->hash % table->bucket_count != i) {
                    return 0;
                }
            }
        }
    }
    return count == table->size;
}

char *hash_table_describe(const hash_table_t *table)
{
    hash_table_stats stats;
    if (!hash_table_snapshot(table, &stats, NULL, 0) || stats.max_bucket_size > (SIZE_MAX - 4096) / 32 - 1) {
        return NULL;
    }
    const size_t bins = stats.max_bucket_size + 1;
    size_t *histogram = malloc(bins * sizeof(*histogram));
    char *json = malloc(4096 + bins * 32);
    if (!histogram || !json) {
        free(histogram);
        free(json);
        return NULL;
    }
    hash_table_snapshot(table, &stats, histogram, bins);
    char seed[HASH_TABLE_SEED_SIZE * 2 + 1];
    for (size_t i = 0; i < HASH_TABLE_SEED_SIZE; i++) {
        snprintf(seed + i * 2, 3, "%02x", table->seed[i]);
    }
    size_t used = (size_t)snprintf(json, 4096,
        "{\"bucket_structure\":\"%s\",\"bucket_support\":[\"list\",\"rbtree\"],"
        "\"config\":{\"initial_buckets\":%zu,\"hash_seed\":\"%s\",\"seed_source\":\"%s\"},"
        "\"stats\":{\"size\":%zu,\"bucket_count\":%zu,\"load_factor\":%.9f,"
        "\"nonempty_buckets\":%zu,\"max_bucket_size\":%zu,\"max_chain_length\":%zu,"
        "\"max_tree_height\":%zu,\"rehash_count\":%llu,\"rehash_ms\":%.9f,\"bucket_size_histogram\":[",
        hash_table_structure_name(table->structure), table->initial_buckets, seed,
        table->explicit_seed ? "explicit" : "os_random", stats.size, stats.bucket_count, stats.load_factor,
        stats.nonempty_buckets, stats.max_bucket_size, stats.max_chain_length, stats.max_tree_height,
        (unsigned long long)stats.rehash_count, stats.rehash_ms);
    for (size_t i = 0; i < bins; i++) {
        used += (size_t)snprintf(json + used, 32, "%s%zu", i ? "," : "", histogram[i]);
    }
    strcpy(json + used, "]}}");
    free(histogram);
    return json;
}

#ifdef HASH_TABLE_TEST_ALLOCATOR
int hash_table_test_force_collisions(hash_table_t *table)
{
    if (!table || table->size) {
        return 0;
    }
    table->test_constant_hash = 1;
    return 1;
}
#endif
