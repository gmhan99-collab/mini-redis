#include "server/redis_http.h"
#include "bucket_options.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    uint64_t port = 8080;
    uint8_t seed[16];
    hash_table_config config = {1024, HASH_TABLE_LIST, NULL};
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
        } else {
            goto invalid;
        }
    }
    return redis_server_run_config((uint16_t)port, &config);
invalid:
    fprintf(stderr, "Usage: %s [port] [--bucket-structure list|rbtree] [--buckets 1..65536] [--hash-seed HEX32]\n", argv[0]);
    return EXIT_FAILURE;
}
