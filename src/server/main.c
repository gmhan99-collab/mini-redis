#include "server/redis_http.h"
#include "bucket_options.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    uint64_t port = 8080;
    uint8_t seed[16];
    hash_table_config config = {1024, HASH_TABLE_LIST, NULL};
    persistence_options options = {PERSISTENCE_NONE, NULL, 0};
    unsigned seen = 0;
    for (int i = 1; i < argc; i++) {
        uint64_t number;
        if (argv[i][0] != '-') {
            if ((seen & 1) || !bucket_number(argv[i], 1, 65535, &port)) {
                goto invalid;
            }
            seen |= 1;
            continue;
        }
        if (i + 1 >= argc) {
            goto invalid;
        }
        const char *option = argv[i++];
        if (!strcmp(option, "--bucket-structure") && !(seen & 2)) {
            if (!bucket_structure(argv[i], &config.bucket_structure)) {
                goto invalid;
            }
            seen |= 2;
        } else if (!strcmp(option, "--buckets") && !(seen & 4)) {
            if (!bucket_number(argv[i], 1, 65536, &number)) {
                goto invalid;
            }
            config.bucket_count = (size_t)number;
            seen |= 4;
        } else if (!strcmp(option, "--hash-seed") && !(seen & 8)) {
            if (!bucket_seed(argv[i], seed)) {
                goto invalid;
            }
            config.seed = seed;
            seen |= 8;
        } else if (!strcmp(option, "--persistence") && !(seen & 16)) {
            if (!strcmp(argv[i], "rdb")) {
                options.mode = PERSISTENCE_RDB;
            } else if (!strcmp(argv[i], "aof")) {
                options.mode = PERSISTENCE_AOF;
            } else if (strcmp(argv[i], "none")) {
                goto invalid;
            }
            seen |= 16;
        } else if (!strcmp(option, "--data-dir") && !(seen & 32)) {
            if (!argv[i][0]) {
                goto invalid;
            }
            options.directory = argv[i];
            seen |= 32;
        } else if (!strcmp(option, "--data-action") && !(seen & 64)) {
            if (!strcmp(argv[i], "create")) {
                options.create = 1;
            } else if (strcmp(argv[i], "open")) {
                goto invalid;
            }
            seen |= 64;
        } else {
            goto invalid;
        }
    }
    if ((seen & 96) != (options.mode == PERSISTENCE_NONE ? 0u : 96u)) {
        goto invalid;
    }
    return redis_server_run_managed((uint16_t)port, &config, &options);
invalid:
    fprintf(stderr, "Usage: %s [port] [--bucket-structure list|rbtree] [--buckets 1..65536] [--hash-seed HEX32]\n"
                    "       [--persistence none|rdb|aof] [--data-dir PATH --data-action create|open]\n", argv[0]);
    return EXIT_FAILURE;
}
