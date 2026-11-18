/* Minimal store.h implementation so HTTP tests can run before the real store is linked.
 * Expiry rule mirrors the contract: a key is expired once now_ms >= stored_at + ttl_ms. */
#include "store.h"

#include <stdlib.h>
#include <string.h>

typedef struct stub_entry {
    unsigned char *key;
    size_t key_len;
    unsigned char *value;
    size_t value_len;
    uint64_t expires_at;
    struct stub_entry *next;
} stub_entry;

struct store {
    stub_entry *head;
};

store_status store_create(size_t bucket_count, store_t **out_store)
{
    store_t *store;
    (void)bucket_count;
    if (out_store == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    store = calloc(1, sizeof(*store));
    if (store == NULL) {
        return STORE_NO_MEMORY;
    }
    *out_store = store;
    return STORE_OK;
}

static void free_entry(stub_entry *entry)
{
    free(entry->key);
    free(entry->value);
    free(entry);
}

void store_destroy(store_t *store)
{
    stub_entry *entry;
    if (store == NULL) {
        return;
    }
    entry = store->head;
    while (entry != NULL) {
        stub_entry *next = entry->next;
        free_entry(entry);
        entry = next;
    }
    free(store);
}

static stub_entry **find_link(store_t *store, const void *key, size_t key_len)
{
    stub_entry **link = &store->head;
    while (*link != NULL) {
        if ((*link)->key_len == key_len && memcmp((*link)->key, key, key_len) == 0) {
            return link;
        }
        link = &(*link)->next;
    }
    return link;
}

static int remove_if_expired(stub_entry **link, uint64_t now_ms)
{
    stub_entry *entry = *link;
    if (entry->expires_at == 0 || now_ms < entry->expires_at) {
        return 0;
    }
    *link = entry->next;
    free_entry(entry);
    return 1;
}

static unsigned char *copy_bytes(const void *bytes, size_t length)
{
    unsigned char *copy = malloc(length > 0 ? length : 1);
    if (copy != NULL && length > 0) {
        memcpy(copy, bytes, length);
    }
    return copy;
}

store_status store_put(store_t *store, const void *key, size_t key_len,
                       const void *value, size_t value_len, uint64_t ttl_ms, uint64_t now_ms)
{
    stub_entry **link;
    stub_entry *entry;
    unsigned char *value_copy;
    if (store == NULL || (key == NULL && key_len > 0) || (value == NULL && value_len > 0)) {
        return STORE_INVALID_ARGUMENT;
    }
    if (ttl_ms > UINT64_MAX - now_ms) {
        return STORE_OVERFLOW;
    }
    value_copy = copy_bytes(value, value_len);
    if (value_copy == NULL) {
        return STORE_NO_MEMORY;
    }
    link = find_link(store, key, key_len);
    entry = *link;
    if (entry == NULL) {
        entry = calloc(1, sizeof(*entry));
        if (entry == NULL || (entry->key = copy_bytes(key, key_len)) == NULL) {
            free(entry);
            free(value_copy);
            return STORE_NO_MEMORY;
        }
        entry->key_len = key_len;
        entry->next = store->head;
        store->head = entry;
    }
    free(entry->value);
    entry->value = value_copy;
    entry->value_len = value_len;
    entry->expires_at = ttl_ms == 0 ? 0 : now_ms + ttl_ms;
    return STORE_OK;
}

store_status store_get(store_t *store, const void *key, size_t key_len,
                       uint64_t now_ms, const void **out_value, size_t *out_value_len)
{
    stub_entry **link;
    if (store == NULL || out_value == NULL || out_value_len == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    link = find_link(store, key, key_len);
    if (*link == NULL) {
        return STORE_NOT_FOUND;
    }
    if (remove_if_expired(link, now_ms)) {
        return STORE_EXPIRED;
    }
    *out_value = (*link)->value;
    *out_value_len = (*link)->value_len;
    return STORE_OK;
}

store_status store_delete(store_t *store, const void *key, size_t key_len, uint64_t now_ms)
{
    stub_entry **link;
    stub_entry *entry;
    if (store == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    link = find_link(store, key, key_len);
    if (*link == NULL) {
        return STORE_NOT_FOUND;
    }
    if (remove_if_expired(link, now_ms)) {
        return STORE_EXPIRED;
    }
    entry = *link;
    *link = entry->next;
    free_entry(entry);
    return STORE_OK;
}

const char *store_status_message(store_status status)
{
    (void)status;
    return "stub";
}
