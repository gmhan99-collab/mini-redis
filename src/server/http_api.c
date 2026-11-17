#include "server/http_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llhttp.h"

/* Only these header names affect request handling; longer names are never interesting. */
#define HTTP_FIELD_NAME_CAPACITY 32
#define HTTP_RESPONSE_HEAD_CAPACITY 256

typedef enum {
    HEADER_OTHER,
    HEADER_HOST,
    HEADER_CONTENT_LENGTH,
    HEADER_TRANSFER_ENCODING,
    HEADER_EXPECT,
    HEADER_UPGRADE,
    HEADER_TTL,
    HEADER_CONTENT_TYPE
} header_kind;

static const http_error ERROR_BAD_REQUEST = {400, "bad_request", NULL};
static const http_error ERROR_INVALID_HOST = {400, "invalid_host", NULL};
static const http_error ERROR_EXPECT = {400, "expect_not_supported", NULL};
static const http_error ERROR_TRANSFER_ENCODING = {400, "unsupported_transfer_encoding", NULL};
static const http_error ERROR_DUPLICATE_LENGTH = {400, "invalid_content_length", NULL};
static const http_error ERROR_UPGRADE = {400, "upgrade_not_supported", NULL};
static const http_error ERROR_BODY_TOO_LARGE = {413, "payload_too_large", NULL};
static const http_error ERROR_HEADERS_TOO_LARGE = {431, "headers_too_large", NULL};
static const http_error ERROR_INTERNAL = {500, "internal_error", NULL};
static const http_error ERROR_VERSION = {505, "http_version_not_supported", NULL};

static const char JSON_CONTENT_TYPE[] = "application/json";
struct http_connection {
    llhttp_t parser;
    llhttp_settings_t settings;
    uint64_t now_ms;
    size_t header_bytes;
    int headers_complete;
    int message_complete;
    int responded;
    int keep_alive;
    const http_error *error;

    char url[HTTP_MAX_HEADER_BYTES];
    size_t url_length;
    char field[HTTP_FIELD_NAME_CAPACITY];
    size_t field_length;
    int field_too_long;
    header_kind current_header;
    char value[HTTP_MAX_HEADER_BYTES];
    size_t value_length;

    unsigned int host_count;
    int host_empty;
    unsigned int content_length_count;
    unsigned int ttl_count;
    char ttl[HTTP_MAX_HEADER_BYTES];
    size_t ttl_length;
    unsigned int content_type_count;
    char content_type[HTTP_MAX_HEADER_BYTES];
    size_t content_type_length;
    const http_handler *handler;
    void *context;

    uint8_t method;
    unsigned char *body;
    size_t body_length;
    size_t body_capacity;
};

static http_request request_view(const http_connection_t *connection);

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Content Too Large";
    case 415:
        return "Unsupported Media Type";
    case 422:
        return "Unprocessable Content";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

/* Builds one owned buffer that survives request reset and application mutations. */
int http_build_response(http_response *response, int status, const char *content_type,
                          const void *body, size_t body_length, const char *allow,
                          int close_after)
{
    char head[HTTP_RESPONSE_HEAD_CAPACITY];
    int head_length = snprintf(head, sizeof(head),
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %llu\r\n"
                               "Cache-Control: no-store\r\n"
                               "%s%s%s"
                               "%s"
                               "\r\n",
                               status, reason_phrase(status), content_type,
                               (unsigned long long)body_length,
                               allow != NULL ? "Allow: " : "",
                               allow != NULL ? allow : "",
                               allow != NULL ? "\r\n" : "",
                               close_after ? "Connection: close\r\n" : "");
    unsigned char *data;
    if (head_length < 0 || (size_t)head_length >= sizeof(head)) {
        return -1;
    }
    if (body_length > SIZE_MAX - (size_t)head_length) {
        return -1;
    }
    data = malloc((size_t)head_length + body_length);
    if (data == NULL) {
        return -1;
    }
    memcpy(data, head, (size_t)head_length);
    if (body_length > 0) {
        memcpy(data + head_length, body, body_length);
    }
    response->data = data;
    response->length = (size_t)head_length + body_length;
    response->close_after = close_after;
    return 1;
}

int http_build_json(http_response *response, int status, const char *json,
                               const char *allow, int close_after)
{
    return http_build_response(response, status, JSON_CONTENT_TYPE, json, strlen(json), allow,
                          close_after);
}

/* Every protocol or validation error closes the connection after the response. */
int http_build_error(http_response *response, const http_error *error)
{
    char json[64];
    int json_length = snprintf(json, sizeof(json), "{\"error\":\"%s\"}", error->code);
    if (json_length < 0 || (size_t)json_length >= sizeof(json)) {
        return -1;
    }
    return http_build_json(response, error->status, json, error->allow, 1);
}

static int fail_request(http_connection_t *connection, const http_error *error)
{
    if (connection->error == NULL) {
        connection->error = error;
    }
    return -1;
}

static int is_space_or_tab(char c)
{
    return c == ' ' || c == '\t';
}

static int equals_ignore_case(const char *text, size_t length, const char *expected)
{
    size_t i;
    if (strlen(expected) != length) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static header_kind classify_header(const http_connection_t *connection)
{
    const char *name = connection->field;
    size_t length = connection->field_length;
    if (connection->field_too_long) {
        return HEADER_OTHER;
    }
    if (equals_ignore_case(name, length, "host")) {
        return HEADER_HOST;
    }
    if (equals_ignore_case(name, length, "content-length")) {
        return HEADER_CONTENT_LENGTH;
    }
    if (equals_ignore_case(name, length, "transfer-encoding")) {
        return HEADER_TRANSFER_ENCODING;
    }
    if (equals_ignore_case(name, length, "expect")) {
        return HEADER_EXPECT;
    }
    if (equals_ignore_case(name, length, "upgrade")) {
        return HEADER_UPGRADE;
    }
    if (equals_ignore_case(name, length, "content-type")) {
        return HEADER_CONTENT_TYPE;
    }
    if (equals_ignore_case(name, length, "x-ttl-ms")) {
        return HEADER_TTL;
    }
    return HEADER_OTHER;
}

static int on_url(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (length > sizeof(connection->url) - connection->url_length) {
        return fail_request(connection, &ERROR_HEADERS_TOO_LARGE);
    }
    memcpy(connection->url + connection->url_length, at, length);
    connection->url_length += length;
    return 0;
}

static int on_header_field(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (connection->field_too_long ||
        length > sizeof(connection->field) - connection->field_length) {
        connection->field_too_long = 1;
        return 0;
    }
    memcpy(connection->field + connection->field_length, at, length);
    connection->field_length += length;
    return 0;
}

static int on_header_field_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    connection->current_header = classify_header(connection);
    connection->field_length = 0;
    connection->field_too_long = 0;
    connection->value_length = 0;
    switch (connection->current_header) {
    case HEADER_TRANSFER_ENCODING:
        return fail_request(connection, &ERROR_TRANSFER_ENCODING);
    case HEADER_EXPECT:
        return fail_request(connection, &ERROR_EXPECT);
    case HEADER_UPGRADE:
        /* llhttp flags upgrade only with "Connection: upgrade"; any Upgrade is refused. */
        return fail_request(connection, &ERROR_UPGRADE);
    case HEADER_CONTENT_LENGTH:
        connection->content_length_count++;
        if (connection->content_length_count > 1) {
            return fail_request(connection, &ERROR_DUPLICATE_LENGTH);
        }
        break;
    case HEADER_HOST:
        connection->host_count++;
        if (connection->host_count > 1) {
            return fail_request(connection, &ERROR_INVALID_HOST);
        }
        break;
    case HEADER_TTL:
        connection->ttl_count++;
        break;
    case HEADER_CONTENT_TYPE:
        connection->content_type_count++;
        break;
    default:
        break;
    }
    if (connection->handler && connection->handler->validate_header) {
        http_request request = request_view(connection);
        const http_error *error = connection->handler->validate_header(&request, connection->context);
        if (error) {
            return fail_request(connection, error);
        }
    }
    return 0;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (connection->current_header != HEADER_HOST && connection->current_header != HEADER_TTL &&
        connection->current_header != HEADER_CONTENT_TYPE) {
        return 0;
    }
    if (length > sizeof(connection->value) - connection->value_length) {
        return fail_request(connection, &ERROR_HEADERS_TOO_LARGE);
    }
    memcpy(connection->value + connection->value_length, at, length);
    connection->value_length += length;
    return 0;
}

static int on_header_value_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    const char *value = connection->value;
    size_t length = connection->value_length;
    while (length > 0 && is_space_or_tab(value[0])) {
        value++;
        length--;
    }
    while (length > 0 && is_space_or_tab(value[length - 1])) {
        length--;
    }
    if (connection->current_header == HEADER_HOST) {
        connection->host_empty = length == 0;
    } else if (connection->current_header == HEADER_TTL) {
        memcpy(connection->ttl, value, length);
        connection->ttl_length = length;
    } else if (connection->current_header == HEADER_CONTENT_TYPE) {
        memcpy(connection->content_type, value, length);
        connection->content_type_length = length;
    }
    connection->current_header = HEADER_OTHER;
    connection->value_length = 0;
    return 0;
}

static http_request request_view(const http_connection_t *connection)
{
    http_request request = {0};
    request.url = connection->url;
    request.url_length = connection->url_length;
    request.method = connection->method;
    request.body = connection->body;
    request.body_length = connection->body_length;
    request.content_length = connection->parser.content_length;
    request.content_length_count = connection->content_length_count;
    request.now_ms = connection->now_ms;
    request.ttl_count = connection->ttl_count;
    request.ttl = connection->ttl;
    request.ttl_length = connection->ttl_length;
    request.content_type_count = connection->content_type_count;
    request.content_type = connection->content_type;
    request.content_type_length = connection->content_type_length;
    request.keep_alive = connection->keep_alive;
    return request;
}

static const http_error *validate_request(http_connection_t *connection)
{
    llhttp_t *parser = &connection->parser;
    http_request request;
    if (llhttp_get_http_major(parser) != 1 || llhttp_get_http_minor(parser) != 1) {
        return &ERROR_VERSION;
    }
    if (llhttp_get_upgrade(parser)) {
        return &ERROR_UPGRADE;
    }
    if (connection->host_count != 1 || connection->host_empty) {
        return &ERROR_INVALID_HOST;
    }
    connection->method = llhttp_get_method(parser);
    request = request_view(connection);
    if (connection->handler && connection->handler->validate) {
        const http_error *error = connection->handler->validate(&request, connection->context);
        if (error) {
            return error;
        }
    }
    return parser->content_length > HTTP_MAX_BODY_BYTES ? &ERROR_BODY_TOO_LARGE : NULL;
}

/* The whole body is bounded by HTTP_MAX_BODY_BYTES, so it is allocated once up front. */
static int on_headers_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    const http_error *error = validate_request(connection);
    connection->headers_complete = 1;
    if (error != NULL) {
        return fail_request(connection, error);
    }
    if (parser->content_length > 0) {
        connection->body = malloc((size_t)parser->content_length);
        if (connection->body == NULL) {
            return fail_request(connection, &ERROR_INTERNAL);
        }
        connection->body_capacity = (size_t)parser->content_length;
    }
    return 0;
}

static int on_body(llhttp_t *parser, const char *at, size_t length)
{
    http_connection_t *connection = parser->data;
    if (length > connection->body_capacity - connection->body_length) {
        return fail_request(connection, &ERROR_BAD_REQUEST);
    }
    memcpy(connection->body + connection->body_length, at, length);
    connection->body_length += length;
    return 0;
}

/* Pausing here leaves pipelined bytes unparsed until the response has been sent. */
static int on_message_complete(llhttp_t *parser)
{
    http_connection_t *connection = parser->data;
    connection->message_complete = 1;
    connection->keep_alive = llhttp_should_keep_alive(parser);
    return HPE_PAUSED;
}

static void clear_request(http_connection_t *connection)
{
    free(connection->body);
    connection->body = NULL;
    connection->body_length = 0;
    connection->body_capacity = 0;
    connection->header_bytes = 0;
    connection->headers_complete = 0;
    connection->message_complete = 0;
    connection->responded = 0;
    connection->keep_alive = 0;
    connection->error = NULL;
    connection->url_length = 0;
    connection->field_length = 0;
    connection->field_too_long = 0;
    connection->current_header = HEADER_OTHER;
    connection->value_length = 0;
    connection->host_count = 0;
    connection->host_empty = 0;
    connection->content_length_count = 0;
    connection->ttl_count = 0;
    connection->ttl_length = 0;
    connection->content_type_count = 0;
    connection->content_type_length = 0;
}

http_connection_t *http_connection_create(void)
{
    http_connection_t *connection = calloc(1, sizeof(*connection));
    if (connection == NULL) {
        return NULL;
    }
    /* No llhttp_set_lenient_* call: ambiguous framing must stay a parse error. */
    llhttp_settings_init(&connection->settings);
    connection->settings.on_url = on_url;
    connection->settings.on_header_field = on_header_field;
    connection->settings.on_header_field_complete = on_header_field_complete;
    connection->settings.on_header_value = on_header_value;
    connection->settings.on_header_value_complete = on_header_value_complete;
    connection->settings.on_headers_complete = on_headers_complete;
    connection->settings.on_body = on_body;
    connection->settings.on_message_complete = on_message_complete;
    llhttp_init(&connection->parser, HTTP_REQUEST, &connection->settings);
    connection->parser.data = connection;
    clear_request(connection);
    return connection;
}

void http_connection_destroy(http_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    free(connection->body);
    free(connection);
}

void http_connection_reset(http_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    clear_request(connection);
    llhttp_reset(&connection->parser);
}

static int respond_with_error(http_connection_t *connection, http_response *response)
{
    const http_error *error = connection->error != NULL ? connection->error : &ERROR_BAD_REQUEST;
    connection->responded = 1;
    return http_build_error(response, error) < 0 ? -1 : 1;
}

/* Feeds at most the remaining header budget until headers finish, so the 8 KiB limit
 * is enforced on raw bytes regardless of how the request was split. */
static size_t next_chunk_length(const http_connection_t *connection, size_t remaining)
{
    size_t allowed;
    if (connection->headers_complete) {
        return remaining;
    }
    allowed = HTTP_MAX_HEADER_BYTES - connection->header_bytes;
    return remaining < allowed ? remaining : allowed;
}

int http_connection_feed(http_connection_t *connection, const void *data, size_t length,
                         size_t *consumed, const http_handler *handler, void *context, uint64_t now_ms,
                         http_response *response)
{
    const char *bytes = data;
    size_t offset = 0;
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (connection == NULL || consumed == NULL || response == NULL ||
        (data == NULL && length > 0)) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    if (connection->responded) {
        return 0;
    }
    connection->now_ms = now_ms;
    connection->handler = handler;
    connection->context = context;
    while (offset < length) {
        size_t chunk = next_chunk_length(connection, length - offset);
        int headers_were_complete = connection->headers_complete;
        llhttp_errno_t parse_error = llhttp_execute(&connection->parser, bytes + offset, chunk);
        if (parse_error == HPE_PAUSED && connection->message_complete) {
            int result;
            *consumed = (size_t)(llhttp_get_error_pos(&connection->parser) - bytes);
            connection->responded = 1;
            http_request request = request_view(connection);
            result = handler && handler->execute ? handler->execute(&request, context, response) : -1;
            return result < 0 ? -1 : 1;
        }
        if (parse_error != HPE_OK) {
            *consumed = length;
            return respond_with_error(connection, response);
        }
        offset += chunk;
        if (!headers_were_complete && !connection->headers_complete) {
            connection->header_bytes += chunk;
            if (connection->header_bytes >= HTTP_MAX_HEADER_BYTES) {
                *consumed = length;
                connection->error = &ERROR_HEADERS_TOO_LARGE;
                return respond_with_error(connection, response);
            }
        }
    }
    *consumed = length;
    return 0;
}
