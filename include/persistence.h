#ifndef MINI_REDIS_PERSISTENCE_H
#define MINI_REDIS_PERSISTENCE_H

#include "store.h"

typedef enum { PERSISTENCE_NONE, PERSISTENCE_RDB, PERSISTENCE_AOF } persistence_mode;
typedef struct {
    persistence_mode mode;
    const char *directory;
    int create;
} persistence_options;

typedef struct persistence persistence;

/* The object owns the store and the directory lock until destroyed. */
persistence *persistence_load(const persistence_options *options, const hash_table_config *config);
void persistence_destroy(persistence *state);
store_t *persistence_store(persistence *state);
persistence_mode persistence_current_mode(const persistence *state);
int persistence_failed(const persistence *state);
int persistence_begin(persistence *state, uint64_t (*monotonic_ms)(void));
uint64_t persistence_now(persistence *state, uint64_t monotonic_ms);
int persistence_finish(persistence *state, uint64_t monotonic_ms, double *elapsed_ms);
int persistence_save(persistence *state, uint64_t now_ms, double *elapsed_ms);
int persistence_status_json(const persistence *state, uint64_t now_ms, char *out, size_t capacity);
store_status persistence_put(persistence *state, const void *key, size_t key_len,
                             const void *value, size_t value_len, uint64_t ttl_ms, uint64_t now_ms);
store_status persistence_delete(persistence *state, const void *key, size_t key_len, uint64_t now_ms);

#endif
