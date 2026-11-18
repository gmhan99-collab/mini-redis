#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hash_table.h"

static int report_failure(const char *operation, hash_table_status status)
{
    fprintf(stderr, "%s failed: %s\n", operation, hash_table_status_message(status));
    return 0;
}

static int set_text(hash_table_t *table, const char *key, const char *value)
{
    hash_table_set_outcome outcome;
    const hash_table_status status = hash_table_set(table, key, strlen(key), value, strlen(value), &outcome);

    if (status != HASH_TABLE_OK) {
        return report_failure("SET", status);
    }
    printf("SET %s %s -> %s\n", key, value, outcome == HASH_TABLE_SET_INSERTED ? "inserted" : "replaced");
    return 1;
}

/* 키가 없는 것은 정상 결과로 출력하고, 그 밖의 상태만 실패로 본다. */
static int print_lookup(const hash_table_t *table, const char *key)
{
    const void *value = NULL;
    size_t value_len = 0;
    const hash_table_status status = hash_table_get(table, key, strlen(key), &value, &value_len);

    if (status == HASH_TABLE_ERR_NOT_FOUND) {
        printf("GET %s -> (not found)\n", key);
        return 1;
    }
    if (status != HASH_TABLE_OK) {
        return report_failure("GET", status);
    }
    printf("GET %s -> %.*s\n", key, (int)value_len, (const char *)value);
    return 1;
}

static int delete_key(hash_table_t *table, const char *key)
{
    const hash_table_status status = hash_table_delete(table, key, strlen(key));

    if (status != HASH_TABLE_OK) {
        return report_failure("DEL", status);
    }
    printf("DEL %s -> ok\n", key);
    return 1;
}

static int run_commands(hash_table_t *table)
{
    return set_text(table, "user:1", "alice")
        && set_text(table, "user:1", "bob")
        && print_lookup(table, "user:1")
        && delete_key(table, "user:1")
        && print_lookup(table, "user:1");
}

int main(void)
{
    hash_table_t *table = NULL;
    const hash_table_status status = hash_table_create(HASH_TABLE_DEFAULT_BUCKET_COUNT, &table);

    if (status != HASH_TABLE_OK) {
        report_failure("CREATE", status);
        return EXIT_FAILURE;
    }
    const int succeeded = run_commands(table);
    if (succeeded) {
        printf("SIZE -> %zu\n", hash_table_size(table));
    }
    hash_table_destroy(table);
    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
