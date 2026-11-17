#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "server/redis_http.h"
#include "build_info.h"
#include "llhttp.h"
#include "measurement_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef enum { ROUTE_HEALTH, ROUTE_KEY } route_kind;
typedef struct {
    const char *url;
    size_t url_length;
    uint8_t method;
    const unsigned char *body;
    size_t body_length;
    uint64_t now_ms, ttl_ms;
    unsigned int ttl_count, content_length_count;
    int ttl_valid, keep_alive;
    struct { uint64_t content_length; } parser;
    route_kind route;
    unsigned char key[HTTP_MAX_KEY_BYTES];
    size_t key_length;
} redis_request;
static const http_error ERROR_INVALID_KEY = {400, "invalid_key", NULL};
static const http_error ERROR_INVALID_TTL = {400, "invalid_ttl", NULL};
static const http_error ERROR_TTL_NOT_ALLOWED = {400, "ttl_not_allowed", NULL};
static const http_error ERROR_INVALID_ARGUMENT = {400, "invalid_argument", NULL};
static const http_error ERROR_ROUTE_NOT_FOUND = {404, "route_not_found", NULL};
static const http_error ERROR_HEALTH_METHOD = {405, "method_not_allowed", "GET"};
static const http_error ERROR_KEY_METHOD = {405, "method_not_allowed", "GET, PUT, DELETE"};
static const http_error ERROR_INVALID_TARGET = {400, "invalid_target", NULL};
static const http_error ERROR_QUERY = {400, "query_not_supported", NULL};
static const http_error ERROR_BODY_TOO_LARGE = {413, "payload_too_large", NULL};
static const http_error ERROR_KEY_TOO_LONG = {414, "key_too_long", NULL};
static const http_error ERROR_INTERNAL = {500, "internal_error", NULL};
static const http_error ERROR_LENGTH_REQUIRED = {400, "content_length_required", NULL};
static const http_error ERROR_BODY_NOT_ALLOWED = {400, "body_not_allowed", NULL};
static const char BINARY_CONTENT_TYPE[] = "application/octet-stream";
static const char JSON_OK[] = "{\"ok\":true}";

/* Accepts only a positive decimal integer that fits in uint64_t. */
static int parse_ttl(const char *text, size_t length, uint64_t *out_ttl)
{
    uint64_t ttl = 0;
    size_t i;
    if (length == 0) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        unsigned int digit;
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        digit = (unsigned int)(text[i] - '0');
        if (ttl > (UINT64_MAX - digit) / 10) {
            return 0;
        }
        ttl = ttl * 10 + digit;
    }
    if (ttl == 0) {
        return 0;
    }
    *out_ttl = ttl;
    return 1;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Percent-decodes exactly once; a decoded "%25" stays a literal '%'. */
static const http_error *decode_key(redis_request *connection, const char *encoded,
                                    size_t length)
{
    size_t i = 0;
    connection->key_length = 0;
    if (length == 0) {
        return &ERROR_INVALID_KEY;
    }
    while (i < length) {
        unsigned char byte = (unsigned char)encoded[i];
        if (byte == '%') {
            int high;
            int low;
            if (length - i < 3) {
                return &ERROR_INVALID_KEY;
            }
            high = hex_value(encoded[i + 1]);
            low = hex_value(encoded[i + 2]);
            if (high < 0 || low < 0) {
                return &ERROR_INVALID_KEY;
            }
            byte = (unsigned char)(high * 16 + low);
            i += 3;
        } else {
            i++;
        }
        if (byte == '\0') {
            return &ERROR_INVALID_KEY;
        }
        if (connection->key_length == sizeof(connection->key)) {
            return &ERROR_KEY_TOO_LONG;
        }
        connection->key[connection->key_length++] = byte;
    }
    return NULL;
}

static const http_error *resolve_route(redis_request *connection)
{
    static const char key_prefix[] = "/keys/";
    const size_t prefix_length = sizeof(key_prefix) - 1;
    const char *url = connection->url;
    size_t length = connection->url_length;
    uint8_t method = connection->method;
    if (length == 0 || url[0] != '/') {
        return &ERROR_INVALID_TARGET;
    }
    if (memchr(url, '?', length) != NULL || memchr(url, '#', length) != NULL) {
        return &ERROR_QUERY;
    }
    if (length == strlen("/health") && memcmp(url, "/health", length) == 0) {
        connection->route = ROUTE_HEALTH;
        return method == HTTP_GET ? NULL : &ERROR_HEALTH_METHOD;
    }
    if (length < prefix_length || memcmp(url, key_prefix, prefix_length) != 0) {
        return &ERROR_ROUTE_NOT_FOUND;
    }
    connection->route = ROUTE_KEY;
    if (method != HTTP_GET && method != HTTP_PUT && method != HTTP_DELETE) {
        return &ERROR_KEY_METHOD;
    }
    return decode_key(connection, url + prefix_length, length - prefix_length);
}

static const http_error *validate_ttl(const redis_request *connection)
{
    if (connection->ttl_count == 0) {
        return NULL;
    }
    if (connection->method != HTTP_PUT) {
        return &ERROR_TTL_NOT_ALLOWED;
    }
    if (!connection->ttl_valid || connection->ttl_ms > UINT64_MAX - connection->now_ms) {
        return &ERROR_INVALID_TTL;
    }
    return NULL;
}

static const http_error *validate_body_length(const redis_request *connection)
{
    uint64_t content_length = connection->parser.content_length;
    if (connection->method == HTTP_PUT && connection->content_length_count == 0) {
        return &ERROR_LENGTH_REQUIRED;
    }
    if (connection->method != HTTP_PUT && content_length > 0) {
        return &ERROR_BODY_NOT_ALLOWED;
    }
    if (content_length > HTTP_MAX_BODY_BYTES) {
        return &ERROR_BODY_TOO_LARGE;
    }
    return NULL;
}

static int respond_store_status(http_response *response, store_status status, int has_ttl,
                                int close_after)
{
    switch (status) {
    case STORE_OK:
        return http_build_json(response, 200, JSON_OK, NULL, close_after);
    case STORE_NOT_FOUND:
        return http_build_json(response, 404, "{\"error\":\"not_found\"}", NULL,
                                   close_after);
    case STORE_EXPIRED:
        return http_build_json(response, 404, "{\"error\":\"expired\"}", NULL,
                                   close_after);
    case STORE_INVALID_ARGUMENT:
        return http_build_error(response, &ERROR_INVALID_ARGUMENT);
    case STORE_OVERFLOW:
        return http_build_error(response, has_ttl ? &ERROR_INVALID_TTL : &ERROR_INTERNAL);
    default:
        return http_build_error(response, &ERROR_INTERNAL);
    }
}

static int execute_key_request(redis_request *connection, store_t *store,
                               http_response *response)
{
    int close_after = !connection->keep_alive;
    store_status status;
    uint64_t start, end;
    double elapsed_ms;
    int result;
    if (connection->method == HTTP_GET) {
        const void *value = NULL;
        size_t value_length = 0;
        start = measurement_ticks();
        status = store_get(store, connection->key, connection->key_length,
                           connection->now_ms, &value, &value_length);
        end = measurement_ticks();
        elapsed_ms = measurement_ms(start, end);
        if (status == STORE_OK) {
            result = http_build_response(response, 200, BINARY_CONTENT_TYPE, value, value_length,
                                  NULL, close_after);
        } else {
            result = respond_store_status(response, status, 0, close_after);
        }
    } else if (connection->method == HTTP_PUT) {
        uint64_t ttl_ms = connection->ttl_count > 0 ? connection->ttl_ms : 0;
        start = measurement_ticks();
        status = store_put(store, connection->key, connection->key_length, connection->body,
                           connection->body_length, ttl_ms, connection->now_ms);
        end = measurement_ticks();
        elapsed_ms = measurement_ms(start, end);
        result = respond_store_status(response, status, ttl_ms != 0, close_after);
    } else {
        start = measurement_ticks();
        status = store_delete(store, connection->key, connection->key_length, connection->now_ms);
        end = measurement_ticks();
        elapsed_ms = measurement_ms(start, end);
        result = respond_store_status(response, status, 0, close_after);
    }
    if (result == 1) {
        /* Insert into the already owned response, never inspect the raw body as text. */
        char timing[96];
        int length = snprintf(timing, sizeof(timing), "Server-Timing: engine;dur=%.9f\r\n", elapsed_ms);
        size_t offset;
        unsigned char *data = NULL;
        for (offset = 0; offset + 3 < response->length; offset++) {
            if (!memcmp(response->data + offset, "\r\n\r\n", 4)) {
                offset += 2;
                if (length > 0 && (size_t)length < sizeof(timing) &&
                    (size_t)length <= SIZE_MAX - response->length) {
                    data = realloc(response->data, response->length + (size_t)length);
                }
                break;
            }
        }
        if (!data) {
            free(response->data);
            response->data = NULL;
            response->length = 0;
            return -1;
        }
        memmove(data + offset + length, data + offset, response->length - offset);
        memcpy(data + offset, timing, (size_t)length);
        response->data = data;
        response->length += (size_t)length;
    }
    return result;
}

static int execute_request(redis_request *connection, store_t *store,
                           http_response *response)
{
    if (connection->route == ROUTE_HEALTH) {
        char *description = store_describe(store);
        if (!description) {
            return http_build_error(response, &ERROR_INTERNAL);
        }
        const size_t capacity = strlen(description) + sizeof(BUILD_INFO_JSON) + 64;
        char *json = malloc(capacity);
        if (!json) {
            free(description);
            return http_build_error(response, &ERROR_INTERNAL);
        }
        snprintf(json, capacity, "{\"ok\":true,\"build_info\":%s,%s", BUILD_INFO_JSON, description + 1);
        const int result = http_build_json(response, 200, json, NULL, !connection->keep_alive);
        free(description);
        free(json);
        return result;
    }
    return execute_key_request(connection, store, response);
}


static void read_request(redis_request *out, const http_request *in)
{
    memset(out, 0, sizeof(*out));
    out->url = in->url;
    out->url_length = in->url_length;
    out->method = in->method;
    out->body = in->body;
    out->body_length = in->body_length;
    out->now_ms = in->now_ms;
    out->ttl_count = in->ttl_count;
    out->ttl_valid = parse_ttl(in->ttl, in->ttl_length, &out->ttl_ms);
    out->content_length_count = in->content_length_count;
    out->parser.content_length = in->content_length;
    out->keep_alive = in->keep_alive;
}
static const http_error *redis_validate(const http_request *in, void *context)
{
    redis_request request;
    const http_error *error;
    (void)context;
    read_request(&request, in);
    if (request.ttl_count > 1) {
        return &ERROR_INVALID_TTL;
    }
    error = resolve_route(&request);
    if (!error) {
        error = validate_ttl(&request);
    }
    if (!error) {
        error = validate_body_length(&request);
    }
    return error;
}
static int redis_execute(const http_request *in, void *context, http_response *response)
{
    redis_request request;
    read_request(&request, in);
    resolve_route(&request);
    return execute_request(&request, context, response);
}
static const http_error *redis_validate_header(const http_request *request, void *context)
{
    (void)context;
    return request->ttl_count > 1 ? &ERROR_INVALID_TTL : NULL;
}
const http_handler redis_http_handler = {redis_validate, redis_execute, redis_validate_header};
