/* Deliberately includes the implementation to corrupt private nodes safely in
 * an isolated test executable; no mutation hook exists in the public library. */
#define HASH_TABLE_TEST_ALLOCATOR
#define HASH_TABLE_TEST_CLOCK
#include "../src/hash_table/hash_table.c"
#include "test_allocator.h"
#include <assert.h>

static uint64_t clock_ticks, clock_step, clock_reads;

uint64_t hash_table_test_clock_ticks(void)
{
    clock_reads++;
    clock_ticks += clock_step;
    return clock_ticks;
}

double hash_table_test_clock_ms(uint64_t start, uint64_t end)
{
    return (double)(end - start) * 0.25;
}

static hash_table_t *make_tree(size_t count)
{
    static const uint8_t seed[16] = {0};
    const hash_table_config config = {16, HASH_TABLE_RBTREE, seed};
    hash_table_t *table;
    assert(hash_table_create_config(&config, &table) == HASH_TABLE_OK);
    assert(hash_table_test_force_collisions(table));
    for (unsigned char key = 1; key <= count; key++) {
        assert(hash_table_set(table, &key, 1, &key, 1, NULL) == HASH_TABLE_OK);
    }
    assert(hash_table_validate(table));
    return table;
}

static void reject_corruption(void)
{
    hash_table_t *table = make_tree(1);
    hash_table_entry *root = table->buckets[0];
    /* NULL children are black, so only the explicit root-black rule can fail. */
    assert(root && !root->left && !root->right && !root->red);
    root->red = 1;
    assert(!hash_table_validate(table));
    root->red = 0;
    assert(hash_table_validate(table));
    hash_table_destroy(table);

    table = make_tree(3);
    root = table->buckets[0];
    assert(root && root->left && root->right);
    assert(!root->red && root->left->red && root->right->red);

    root->left->parent = root->right;
    assert(!hash_table_validate(table));
    root->left->parent = root;
    assert(hash_table_validate(table));

    const uint8_t saved_key = root->left->key[0];
    root->left->key[0] = 4; /* All hashes equal: only key order is now wrong. */
    assert(!hash_table_validate(table));
    root->left->key[0] = saved_key;
    assert(hash_table_validate(table));

    /* Only black height is violated: root/parents/order/red-red remain valid.
     * A mutant that removes left != right must fail this assertion. */
    root->left->red = 0;
    assert(!hash_table_validate(table));
    root->left->red = 1;
    assert(hash_table_validate(table));

    table->buckets[0] = NULL;
    table->buckets[1] = root;
    assert(!hash_table_validate(table));
    table->buckets[1] = NULL;
    table->buckets[0] = root;
    assert(hash_table_validate(table));

    hash_table_entry *saved_left = root->left->left;
    root->left->left = root; /* Cycle; validator must return, never traverse forever. */
    assert(!hash_table_validate(table));
    root->left->left = saved_left;
    assert(hash_table_validate(table));
    hash_table_destroy(table);

    table = make_tree(7);
    root = table->buckets[0];
    assert(root->right && root->right->left);
    const int parent_red = root->right->red;
    const int child_red = root->right->left->red;
    root->right->red = 1;
    root->right->left->red = 1;
    assert(!hash_table_validate(table));
    root->right->red = parent_red;
    root->right->left->red = child_red;
    assert(hash_table_validate(table));
    hash_table_destroy(table);
    puts("Validator negative cases: root, parent, order, black-height, red-red, bucket, cycle rejected and restored");
}

static void expect_accounting(hash_table_t *table, uint64_t count, double duration, uint64_t reads)
{
    hash_table_stats stats;
    assert(hash_table_snapshot(table, &stats, NULL, 0));
    assert(stats.rehash_count == count);
    assert(stats.rehash_ms == duration);
    assert(clock_reads == reads);
    assert(hash_table_validate(table));
}

static void deterministic_rehash_accounting(hash_table_bucket_structure structure)
{
    const uint8_t seed[16] = {0};
    const hash_table_config config = {1, structure, seed};
    hash_table_t *table;
    clock_ticks = clock_reads = 0;
    clock_step = 4;
    assert(hash_table_create_config(&config, &table) == HASH_TABLE_OK);
    expect_accounting(table, 0, 0, 0);
    assert(hash_table_set(table, "a", 1, "v", 1, NULL) == HASH_TABLE_OK);
    assert(hash_table_set(table, "a", 1, "replacement", 11, NULL) == HASH_TABLE_OK);
    const void *value;
    size_t length;
    assert(hash_table_get(table, "a", 1, &value, &length) == HASH_TABLE_OK);
    expect_accounting(table, 0, 0, 0);
    assert(hash_table_set(table, "b", 1, "v", 1, NULL) == HASH_TABLE_OK);
    expect_accounting(table, 1, 1.0, 2);
    clock_step = 12;
    assert(hash_table_set(table, "c", 1, "v", 1, NULL) == HASH_TABLE_OK);
    expect_accounting(table, 2, 4.0, 4);
    assert(hash_table_set(table, "d", 1, "v", 1, NULL) == HASH_TABLE_OK);
    expect_accounting(table, 2, 4.0, 4);

    /* A failed bucket-array allocation reads a start tick but commits nothing. */
    test_allocator_fail_nth(4);
    assert(hash_table_set(table, "e", 1, "v", 1, NULL) == HASH_TABLE_ERR_NO_MEMORY);
    test_allocator_fail_nth(0);
    expect_accounting(table, 2, 4.0, 5);
    clock_step = 8;
    assert(hash_table_set(table, "e", 1, "v", 1, NULL) == HASH_TABLE_OK);
    expect_accounting(table, 3, 6.0, 7);
    assert(hash_table_delete(table, "a", 1) == HASH_TABLE_OK);
    expect_accounting(table, 3, 6.0, 7);
    hash_table_destroy(table);
}

int main(void)
{
    reject_corruption();
    deterministic_rehash_accounting(HASH_TABLE_LIST);
    deterministic_rehash_accounting(HASH_TABLE_RBTREE);
    assert(test_allocator_live_count() == 0 && test_allocator_error_count() == 0);
    puts("Deterministic rehash counts/duration accumulation and no-rehash/OOM stability passed; allocator 0 live/errors");
    return 0;
}
