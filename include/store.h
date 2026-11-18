#ifndef MINI_REDIS_STORE_H
#define MINI_REDIS_STORE_H
#include <stddef.h>
#include <stdint.h>
#include "hash_table.h"
typedef struct store store_t;
typedef enum {
    STORE_OK = 0, STORE_NOT_FOUND, STORE_EXPIRED, STORE_INVALID_ARGUMENT,
    STORE_NO_MEMORY, STORE_OVERFLOW, STORE_RANDOM_SOURCE, STORE_PERSISTENCE_ERROR
} store_status;
store_status store_create(size_t bucket_count, store_t **out_store);
store_status store_create_config(const hash_table_config *config, store_t **out_store);
char *store_describe(const store_t *store);
void store_destroy(store_t *store);
/* ttl_ms=0 means no expiry. now_ms is a monotonic timestamp supplied by caller.
 * A key expires when now_ms >= put_now_ms + ttl_ms. Expired keys are removed only on access
 * (get/delete report STORE_EXPIRED once) or overwrite; unaccessed expired keys stay in memory. */
store_status store_put(store_t *store, const void *key, size_t key_len,
                       const void *value, size_t value_len, uint64_t ttl_ms, uint64_t now_ms);
/* Borrowed result, valid until the next mutating store operation. */
store_status store_get(store_t *store, const void *key, size_t key_len,
                       uint64_t now_ms, const void **out_value, size_t *out_value_len);
store_status store_delete(store_t *store, const void *key, size_t key_len, uint64_t now_ms);
const char *store_status_message(store_status status);
/* Expiry is an absolute timestamp in the caller's clock, or zero.
 * The callback may read its borrowed slices and must not mutate the store. */
typedef int (*store_visitor)(const void *, size_t, const void *, size_t, uint64_t, void *);
int store_visit_live(const store_t *store, uint64_t now_ms, store_visitor visit, void *context);
#endif
