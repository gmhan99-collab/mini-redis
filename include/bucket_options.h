#ifndef BUCKET_OPTIONS_H
#define BUCKET_OPTIONS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static int bucket_number(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *out)
{
    uint64_t value = 0;
    if (!*text) {
        return 0;
    }
    for (; *text; text++) {
        if (*text < '0' || *text > '9') {
            return 0;
        }
        const unsigned digit = (unsigned)(*text - '0');
        if (digit > maximum || value > (maximum - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
    }
    if (value < minimum) {
        return 0;
    }
    *out = value;
    return 1;
}
static int bucket_seed(const char *text, uint8_t seed[16])
{
    if (strlen(text) != 32) {
        return 0;
    }
    for (size_t i = 0; i < 16; i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            const char c = text[2 * i + j];
            unsigned digit;
            if (c >= '0' && c <= '9') {
                digit = (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = (unsigned)(c - 'A' + 10);
            } else {
                return 0;
            }
            value = value * 16 + digit;
        }
        seed[i] = (uint8_t)value;
    }
    return 1;
}
static int bucket_structure(const char *text, hash_table_bucket_structure *out)
{
    if (!strcmp(text, "list")) {
        *out = HASH_TABLE_LIST;
    } else if (!strcmp(text, "rbtree")) {
        *out = HASH_TABLE_RBTREE;
    } else {
        return 0;
    }
    return 1;
}
#endif
