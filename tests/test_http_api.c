#include "server/redis_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(condition) check_condition((condition), #condition, __FILE__, __LINE__)

static int check_condition(int condition, const char *text, const char *file, int line)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, text);
    }
    return condition;
}

typedef struct {
    int result;
    size_t consumed;
    http_response response;
} feed_outcome;

static feed_outcome feed_bytes(http_connection_t *connection, store_t *store, uint64_t now,
                               const void *data, size_t length)
{
    feed_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    outcome.result = http_connection_feed(connection, data, length, &outcome.consumed, &redis_http_handler, store,
                                          now, &outcome.response);
    return outcome;
}

static feed_outcome feed_text(http_connection_t *connection, store_t *store, uint64_t now,
                              const char *text)
{
    return feed_bytes(connection, store, now, text, strlen(text));
}

static int response_status(const http_response *response)
{
    int status = 0;
    if (response->data == NULL || response->length < 12) {
        return 0;
    }
    sscanf((const char *)response->data, "HTTP/1.1 %d", &status);
    return status;
}

static const unsigned char *find_bytes(const unsigned char *data, size_t length,
                                       const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t i;
    for (i = 0; needle_length <= length && i <= length - needle_length; i++) {
        if (memcmp(data + i, needle, needle_length) == 0) {
            return data + i;
        }
    }
    return NULL;
}

static size_t head_length(const http_response *response)
{
    const unsigned char *end = find_bytes(response->data, response->length, "\r\n\r\n");
    return end == NULL ? 0 : (size_t)(end - response->data) + 4;
}

static int has_header(const http_response *response, const char *line)
{
    return find_bytes(response->data, head_length(response), line) != NULL;
}

static int has_engine_timing(const http_response *response)
{
    const char prefix[] = "Server-Timing: engine;dur=";
    const unsigned char *header = find_bytes(response->data, head_length(response), prefix);
    char number[64];
    size_t length = 0;
    double duration = -1;
    if (!header) {
        return 0;
    }
    header += sizeof(prefix) - 1;
    while (header + length < response->data + head_length(response) &&
           header[length] != '\r' && length + 1 < sizeof(number)) {
        number[length] = (char)header[length];
        length++;
    }
    number[length] = 0;
    return sscanf(number, "%lf", &duration) == 1 && duration >= 0 && duration < 1e12;
}

static int body_equals(const http_response *response, const void *expected, size_t length)
{
    size_t head = head_length(response);
    char content_length[64];
    snprintf(content_length, sizeof(content_length), "Content-Length: %llu\r\n",
             (unsigned long long)length);
    return head > 0 && response->length - head == length &&
           memcmp(response->data + head, expected, length) == 0 &&
           has_header(response, content_length) && has_header(response, "Cache-Control: no-store\r\n");
}

static int body_is(const http_response *response, const char *text)
{
    return body_equals(response, text, strlen(text));
}

/* Sends one complete request on a fresh connection and checks the common response shape. */
static http_response run_request(store_t *store, uint64_t now, const void *data, size_t length)
{
    http_connection_t *connection = http_connection_create();
    feed_outcome outcome = feed_bytes(connection, store, now, data, length);
    CHECK(outcome.result == 1);
    CHECK(outcome.consumed == length);
    CHECK(has_header(&outcome.response, "Cache-Control: no-store\r\n"));
    http_connection_destroy(connection);
    return outcome.response;
}

static http_response run_text(store_t *store, uint64_t now, const char *text)
{
    return run_request(store, now, text, strlen(text));
}

static store_t *new_store(void)
{
    store_t *store = NULL;
    if (store_create(16, &store) != STORE_OK) {
        fprintf(stderr, "store_create failed\n");
        exit(1);
    }
    return store;
}

static void test_health_keeps_connection(void)
{
    store_t *store = new_store();
    http_response response = run_text(store, 0, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 200);
    CHECK(find_bytes(response.data + head_length(&response), response.length - head_length(&response),
                     "{\"ok\":true,\"build_info\":{") != NULL);
    CHECK(find_bytes(response.data, response.length, "\"compiler\":") != NULL);
    CHECK(find_bytes(response.data, response.length, "\"flags\":") != NULL);
    CHECK(find_bytes(response.data, response.length, "\"optimization\":") != NULL);
    CHECK(find_bytes(response.data, response.length, "\"build_timestamp\":") != NULL);
    CHECK(find_bytes(response.data, response.length, "\"source\":") != NULL);
    CHECK(has_header(&response, "Content-Type: application/json\r\n"));
    CHECK(response.close_after == 0);
    free(response.data);

    response = run_text(store, 0, "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(!has_engine_timing(&response));
    CHECK(response_status(&response) == 200);
    CHECK(response.close_after == 1);
    CHECK(has_header(&response, "Connection: close\r\n"));
    free(response.data);
    store_destroy(store);
}

static void test_put_get_delete_binary_and_empty(void)
{
    static const unsigned char binary[] = {'a', 0, '\r', '\n', 0xff, 'z'};
    static const char put_head[] = "PUT /keys/bin HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\n";
    unsigned char request[sizeof(put_head) - 1 + sizeof(binary)];
    store_t *store = new_store();
    http_response response;

    memcpy(request, put_head, sizeof(put_head) - 1);
    memcpy(request + sizeof(put_head) - 1, binary, sizeof(binary));
    response = run_request(store, 0, request, sizeof(request));
    CHECK(has_engine_timing(&response));
    CHECK(response_status(&response) == 200);
    CHECK(body_is(&response, "{\"ok\":true}"));
    free(response.data);

    response = run_text(store, 0, "GET /keys/bin HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(has_engine_timing(&response));
    CHECK(response_status(&response) == 200);
    CHECK(body_equals(&response, binary, sizeof(binary)));
    CHECK(has_header(&response, "Content-Type: application/octet-stream\r\n"));
    CHECK(response.close_after == 0);
    free(response.data);

    response = run_text(store, 0, "PUT /keys/bin HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    CHECK(response_status(&response) == 200);
    free(response.data);
    response = run_text(store, 0, "GET /keys/bin HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 200);
    CHECK(body_equals(&response, "", 0));
    free(response.data);

    response = run_text(store, 0, "DELETE /keys/bin HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(has_engine_timing(&response));
    CHECK(response_status(&response) == 200);
    CHECK(body_is(&response, "{\"ok\":true}"));
    free(response.data);

    response = run_text(store, 0, "GET /keys/bin HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 404);
    CHECK(body_is(&response, "{\"error\":\"not_found\"}"));
    CHECK(response.close_after == 0);
    free(response.data);
    response = run_text(store, 0, "DELETE /keys/bin HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 404);
    free(response.data);
    store_destroy(store);
}

static void test_ttl_expiry_and_overwrite(void)
{
    store_t *store = new_store();
    http_response response;

    response = run_text(store, 1000,
                        "PUT /keys/t HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 100\r\nContent-Length: 1\r\n\r\nv");
    CHECK(response_status(&response) == 200);
    free(response.data);
    response = run_text(store, 1099, "GET /keys/t HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 200);
    CHECK(body_is(&response, "v"));
    free(response.data);
    response = run_text(store, 1200, "GET /keys/t HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 404);
    CHECK(has_engine_timing(&response));
    CHECK(body_is(&response, "{\"error\":\"expired\"}"));
    CHECK(response.close_after == 0);
    free(response.data);
    response = run_text(store, 1200, "GET /keys/t HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(body_is(&response, "{\"error\":\"not_found\"}"));
    free(response.data);

    /* Overwriting without X-TTL-Ms removes the earlier expiry. */
    response = run_text(store, 2000,
                        "PUT /keys/t HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 10\r\nContent-Length: 1\r\n\r\na");
    free(response.data);
    response = run_text(store, 2001, "PUT /keys/t HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nb");
    free(response.data);
    response = run_text(store, 900000, "GET /keys/t HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 200);
    CHECK(body_is(&response, "b"));
    free(response.data);

    response = run_text(store, 3000, "DELETE /keys/gone HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 404);
    CHECK(has_engine_timing(&response));
    free(response.data);
    response = run_text(store, 3000,
                        "PUT /keys/d HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 5\r\nContent-Length: 1\r\n\r\nd");
    free(response.data);
    response = run_text(store, 3010, "DELETE /keys/d HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(body_is(&response, "{\"error\":\"expired\"}"));
    free(response.data);
    store_destroy(store);
}

static void test_percent_decoded_key(void)
{
    store_t *store = new_store();
    const void *value = NULL;
    size_t value_length = 0;
    http_response response = run_text(
        store, 0, "PUT /keys/a%20b%2Fc%25 HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nok");
    CHECK(response_status(&response) == 200);
    free(response.data);
    CHECK(store_get(store, "a b/c%", 6, 0, &value, &value_length) == STORE_OK);
    CHECK(value_length == 2 && memcmp(value, "ok", 2) == 0);
    store_destroy(store);
}

/* Byte-at-a-time delivery splits URL, header names/values and body across callbacks. */
static void test_split_delivery_stores_only_when_complete(void)
{
    static const char request[] =
        "PUT /keys/sp%6Cit HTTP/1.1\r\nHost: example\r\nX-TTL-Ms: 5000\r\n"
        "Content-Length: 5\r\n\r\nhello";
    const size_t length = sizeof(request) - 1;
    store_t *store = new_store();
    http_connection_t *connection = http_connection_create();
    const void *value = NULL;
    size_t value_length = 0;
    size_t i;
    int pending_ok = 1;
    feed_outcome outcome;

    for (i = 0; i + 1 < length; i++) {
        outcome = feed_bytes(connection, store, 10, request + i, 1);
        pending_ok = pending_ok && outcome.result == 0 && outcome.consumed == 1 &&
                     outcome.response.data == NULL;
    }
    CHECK(pending_ok);
    CHECK(store_get(store, "split", 5, 10, &value, &value_length) == STORE_NOT_FOUND);
    outcome = feed_bytes(connection, store, 10, request + length - 1, 1);
    CHECK(outcome.result == 1);
    CHECK(outcome.consumed == 1);
    CHECK(response_status(&outcome.response) == 200);
    free(outcome.response.data);
    CHECK(store_get(store, "split", 5, 4000, &value, &value_length) == STORE_OK);
    CHECK(value_length == 5 && memcmp(value, "hello", 5) == 0);
    CHECK(store_get(store, "split", 5, 5010, &value, &value_length) == STORE_EXPIRED);
    http_connection_destroy(connection);
    store_destroy(store);
}

/* The parser stops at the first message boundary and resumes only after reset. */
static void test_pipelined_requests_pause_and_reset(void)
{
    static const char first[] = "PUT /keys/p HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\none";
    static const char second[] = "GET /keys/p HTTP/1.1\r\nHost: x\r\n\r\n";
    char both[sizeof(first) + sizeof(second)];
    store_t *store = new_store();
    http_connection_t *connection = http_connection_create();
    feed_outcome outcome;
    const size_t first_length = sizeof(first) - 1;
    const size_t second_length = sizeof(second) - 1;

    memcpy(both, first, first_length);
    memcpy(both + first_length, second, second_length);
    outcome = feed_bytes(connection, store, 0, both, first_length + second_length);
    CHECK(outcome.result == 1);
    CHECK(outcome.consumed == first_length);
    CHECK(response_status(&outcome.response) == 200);
    free(outcome.response.data);

    /* Before reset the next request must not run. */
    outcome = feed_bytes(connection, store, 0, both + first_length, second_length);
    CHECK(outcome.result == 0);
    CHECK(outcome.consumed == 0);
    CHECK(outcome.response.data == NULL);

    http_connection_reset(connection);
    outcome = feed_bytes(connection, store, 0, both + first_length, second_length);
    CHECK(outcome.result == 1);
    CHECK(outcome.consumed == second_length);
    CHECK(body_is(&outcome.response, "one"));
    free(outcome.response.data);

    /* A response copy stays valid after the stored value is replaced or deleted. */
    http_connection_reset(connection);
    outcome = feed_text(connection, store, 0, second);
    CHECK(outcome.result == 1);
    http_connection_reset(connection);
    {
        feed_outcome put = feed_text(connection, store, 0,
                                     "PUT /keys/p HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\ntwo");
        CHECK(put.result == 1);
        free(put.response.data);
        http_connection_reset(connection);
        put = feed_text(connection, store, 0, "DELETE /keys/p HTTP/1.1\r\nHost: x\r\n\r\n");
        CHECK(put.result == 1);
        free(put.response.data);
    }
    CHECK(body_is(&outcome.response, "one"));
    free(outcome.response.data);
    http_connection_destroy(connection);
    store_destroy(store);
}

typedef struct {
    const char *request;
    int status;
    const char *body;
} error_case;

static void test_rejected_requests_close(void)
{
    static const error_case cases[] = {
        {"GET /health HTTP/1.1\r\n\r\n", 400, "{\"error\":\"invalid_host\"}"},
        {"GET /health HTTP/1.1\r\nHost: \r\n\r\n", 400, "{\"error\":\"invalid_host\"}"},
        {"GET /health HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, "{\"error\":\"invalid_host\"}"},
        {"GET /health HTTP/1.0\r\nHost: x\r\n\r\n", 505, "{\"error\":\"http_version_not_supported\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n\r\n",
         400, "{\"error\":\"unsupported_transfer_encoding\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na",
         400, "{\"error\":\"invalid_content_length\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\na", 400,
         "{\"error\":\"bad_request\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400,
         "{\"error\":\"bad_request\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"content_length_required\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n",
         400, "{\"error\":\"expect_not_supported\"}"},
        {"GET /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nab", 400,
         "{\"error\":\"body_not_allowed\"}"},
        {"DELETE /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\na", 400,
         "{\"error\":\"body_not_allowed\"}"},
        {"GET /health?x=1 HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"query_not_supported\"}"},
        {"GET /keys/a?b HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"query_not_supported\"}"},
        {"GET http://x/health HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"invalid_target\"}"},
        {"GET /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 5\r\n\r\n", 400, "{\"error\":\"ttl_not_allowed\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 0\r\nContent-Length: 0\r\n\r\n", 400,
         "{\"error\":\"invalid_ttl\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: -5\r\nContent-Length: 0\r\n\r\n", 400,
         "{\"error\":\"invalid_ttl\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 1.5\r\nContent-Length: 0\r\n\r\n", 400,
         "{\"error\":\"invalid_ttl\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: \r\nContent-Length: 0\r\n\r\n", 400,
         "{\"error\":\"invalid_ttl\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 18446744073709551616\r\nContent-Length: 0\r\n\r\n",
         400, "{\"error\":\"invalid_ttl\"}"},
        /* Fits uint64_t but now_ms (1) + ttl overflows the expiry time. */
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 18446744073709551615\r\nContent-Length: 0\r\n\r\n",
         400, "{\"error\":\"invalid_ttl\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 5\r\nX-TTL-Ms: 5\r\nContent-Length: 0\r\n\r\n",
         400, "{\"error\":\"invalid_ttl\"}"},
        /* Header-order errors preserve the original immediate duplicate-TTL rejection. */
        {"PUT /keys/a HTTP/1.1\r\nX-TTL-Ms: 5\r\nX-TTL-Ms: 5\r\nContent-Length: 0\r\n\r\n",
         400, "{\"error\":\"invalid_ttl\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nX-TTL-Ms: 5\r\nX-TTL-Ms: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
         400, "{\"error\":\"invalid_ttl\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nX-TTL-Ms: 5\r\nX-TTL-Ms: 5\r\n\r\n",
         400, "{\"error\":\"unsupported_transfer_encoding\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: a\r\nHost: b\r\nX-TTL-Ms: 5\r\nX-TTL-Ms: 5\r\n\r\n",
         400, "{\"error\":\"invalid_host\"}"},
        {"GET /keys/ HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"invalid_key\"}"},
        {"GET /keys/%zz HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"invalid_key\"}"},
        {"GET /keys/a%2 HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"invalid_key\"}"},
        {"GET /keys/a%00 HTTP/1.1\r\nHost: x\r\n\r\n", 400, "{\"error\":\"invalid_key\"}"},
        {"GET /nope HTTP/1.1\r\nHost: x\r\n\r\n", 404, "{\"error\":\"route_not_found\"}"},
        {"GET /keys HTTP/1.1\r\nHost: x\r\n\r\n", 404, "{\"error\":\"route_not_found\"}"},
        {"POST /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 405,
         "{\"error\":\"method_not_allowed\"}"},
        {"PUT /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 405,
         "{\"error\":\"method_not_allowed\"}"},
        {"GET /health HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n", 400,
         "{\"error\":\"upgrade_not_supported\"}"},
        {"GET /health HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n", 400,
         "{\"error\":\"upgrade_not_supported\"}"},
        {"GET /health HTTP/1.1\r\nHost: x\r\nUpgrade:\r\n\r\n", 400,
         "{\"error\":\"upgrade_not_supported\"}"},
        {"PUT /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", 413,
         "{\"error\":\"payload_too_large\"}"},
        {"GARBAGE\r\n\r\n", 400, "{\"error\":\"bad_request\"}"},
        {"GET /health HTTP/1.1\r\nHost x\r\n\r\n", 400, "{\"error\":\"bad_request\"}"},
        {"GET /health HTTP/1.1\nHost: x\n\n", 400, "{\"error\":\"bad_request\"}"},
    };
    store_t *store = new_store();
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        http_response response = run_text(store, 1, cases[i].request);
        int ok = response_status(&response) == cases[i].status &&
                 body_is(&response, cases[i].body) && response.close_after == 1 &&
                 has_header(&response, "Connection: close\r\n");
        if (!CHECK(ok)) {
            fprintf(stderr, "  case %u: got %.*s\n", (unsigned)i, (int)response.length,
                    (const char *)response.data);
        }
        free(response.data);
    }
    {
        const void *value;
        size_t value_length;
        CHECK(store_get(store, "a", 1, 1, &value, &value_length) == STORE_NOT_FOUND);
    }
    store_destroy(store);
}

static void test_duplicate_ttl_fails_at_split_field_before_value(void)
{
    store_t *store = new_store();
    http_connection_t *connection = http_connection_create();
    feed_outcome outcome = feed_text(connection, store, 0,
        "PUT /keys/a HTTP/1.1\r\nX-TTL-Ms: 5\r\nx-tTl-Ms");
    CHECK(outcome.result == 0);
    /* No header value, final CRLF, Host, or later framing error is needed. */
    outcome = feed_text(connection, store, 0, ":");
    CHECK(outcome.result == 1);
    CHECK(response_status(&outcome.response) == 400);
    CHECK(body_is(&outcome.response, "{\"error\":\"invalid_ttl\"}"));
    CHECK(outcome.response.close_after == 1);
    free(outcome.response.data);
    http_connection_destroy(connection);
    store_destroy(store);
}

static void test_method_not_allowed_lists_methods(void)
{
    store_t *store = new_store();
    http_response response = run_text(store, 0, "POST /keys/a HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    CHECK(has_header(&response, "Allow: GET, PUT, DELETE\r\n"));
    free(response.data);
    response = run_text(store, 0, "DELETE /health HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 405);
    CHECK(has_header(&response, "Allow: GET\r\n"));
    free(response.data);
    store_destroy(store);
}

static char *make_key_request(size_t key_length)
{
    static const char head[] = "GET /keys/";
    static const char tail[] = " HTTP/1.1\r\nHost: x\r\n\r\n";
    char *request = malloc(sizeof(head) + key_length + sizeof(tail));
    memcpy(request, head, sizeof(head) - 1);
    memset(request + sizeof(head) - 1, 'k', key_length);
    memcpy(request + sizeof(head) - 1 + key_length, tail, sizeof(tail));
    return request;
}

/* Builds a request whose header area (request line through blank line) is exactly size bytes. */
static char *make_sized_header_request(size_t size)
{
    static const char head[] = "GET /health HTTP/1.1\r\nHost: x\r\nX-Pad: ";
    static const char tail[] = "\r\n\r\n";
    size_t pad = size - (sizeof(head) - 1) - (sizeof(tail) - 1);
    char *request = malloc(size + 1);
    memcpy(request, head, sizeof(head) - 1);
    memset(request + sizeof(head) - 1, 'p', pad);
    memcpy(request + sizeof(head) - 1 + pad, tail, sizeof(tail));
    return request;
}

static void test_size_limits(void)
{
    store_t *store = new_store();
    char *request;
    http_response response;
    static const char body_head[] = "PUT /keys/big HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n";
    size_t big_length = sizeof(body_head) - 1 + HTTP_MAX_BODY_BYTES;
    char *big = malloc(big_length);

    request = make_key_request(HTTP_MAX_KEY_BYTES);
    response = run_text(store, 0, request);
    CHECK(response_status(&response) == 404 && response.close_after == 0);
    free(response.data);
    free(request);
    request = make_key_request(HTTP_MAX_KEY_BYTES + 1);
    response = run_text(store, 0, request);
    CHECK(response_status(&response) == 414 && response.close_after == 1);
    free(response.data);
    free(request);

    request = make_sized_header_request(HTTP_MAX_HEADER_BYTES);
    response = run_text(store, 0, request);
    CHECK(response_status(&response) == 200);
    free(response.data);
    free(request);
    request = make_sized_header_request(HTTP_MAX_HEADER_BYTES + 1);
    response = run_text(store, 0, request);
    CHECK(response_status(&response) == 431 && response.close_after == 1);
    CHECK(body_is(&response, "{\"error\":\"headers_too_large\"}"));
    free(response.data);
    free(request);

    /* An endless header without the blank line is rejected once 8 KiB has arrived. */
    {
        http_connection_t *connection = http_connection_create();
        char filler[1024];
        feed_outcome outcome = feed_text(connection, store, 0, "GET /health HTTP/1.1\r\nX-Long: ");
        int rounds = 0;
        memset(filler, 'f', sizeof(filler));
        while (outcome.result == 0 && rounds < 20) {
            outcome = feed_bytes(connection, store, 0, filler, sizeof(filler));
            rounds++;
        }
        CHECK(outcome.result == 1 && response_status(&outcome.response) == 431);
        CHECK(rounds <= 9);
        free(outcome.response.data);
        http_connection_destroy(connection);
    }

    memcpy(big, body_head, sizeof(body_head) - 1);
    memset(big + sizeof(body_head) - 1, 'b', HTTP_MAX_BODY_BYTES);
    response = run_request(store, 0, big, big_length);
    CHECK(response_status(&response) == 200);
    free(response.data);
    response = run_text(store, 0, "GET /keys/big HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(response_status(&response) == 200);
    CHECK(body_equals(&response, big + sizeof(body_head) - 1, HTTP_MAX_BODY_BYTES));
    free(response.data);
    free(big);
    store_destroy(store);
}

int main(void)
{
    test_health_keeps_connection();
    test_put_get_delete_binary_and_empty();
    test_ttl_expiry_and_overwrite();
    test_percent_decoded_key();
    test_split_delivery_stores_only_when_complete();
    test_pipelined_requests_pause_and_reset();
    test_rejected_requests_close();
    test_duplicate_ttl_fails_at_split_field_before_value();
    test_method_not_allowed_lists_methods();
    test_size_limits();
    if (failures > 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_http_api: %d checks passed\n", checks);
    return 0;
}
