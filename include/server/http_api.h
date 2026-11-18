#ifndef MINI_REDIS_HTTP_API_H
#define MINI_REDIS_HTTP_API_H
#include <stddef.h>
#include <stdint.h>
#define HTTP_MAX_HEADER_BYTES 8192
#define HTTP_MAX_KEY_BYTES 1024
#define HTTP_MAX_BODY_BYTES (1024 * 1024)
typedef struct http_connection http_connection_t;
typedef struct {
    unsigned char *data;
    size_t length;
    int close_after;
    int stop_after;
} http_response;
typedef struct {
    int status;
    const char *code;
    const char *allow;
} http_error;
/* All request slices are borrowed until reset/destroy; handlers must copy responses. */
typedef struct {
    const char *url;
    size_t url_length;
    uint8_t method;
    const unsigned char *body;
    size_t body_length;
    uint64_t content_length, now_ms;
    unsigned int content_length_count, ttl_count, content_type_count;
    const char *ttl, *content_type;
    size_t ttl_length, content_type_length;
    int keep_alive;
} http_request;
typedef struct {
    const http_error *(*validate)(const http_request *, void *);
    int (*execute)(const http_request *, void *, http_response *);
    /* Optional: after each header field is recognized/counted, before its value.
     * Reject immediately to preserve application first-error precedence. */
    const http_error *(*validate_header)(const http_request *, void *);
} http_handler;
http_connection_t *http_connection_create(void);
void http_connection_destroy(http_connection_t *);
/* validate runs once at headers, before allocating the bounded body; it must not
 * execute application work. execute runs only for one completed request.
 * Use the same handler/context for every feed on a connection. Request metadata
 * content_length is meaningful during validate; execute uses body_length.
 * Return: 0 incomplete, 1 owned response ready, -1 fatal allocation/handler error.
 * consumed is always set. Reset only after the owned response is fully sent. */
int http_connection_feed(http_connection_t *, const void *, size_t, size_t *,
                         const http_handler *, void *, uint64_t, http_response *);
void http_connection_reset(http_connection_t *);
int http_build_response(http_response *, int, const char *, const void *, size_t, const char *, int);
int http_build_json(http_response *, int, const char *, const char *, int);
int http_build_error(http_response *, const http_error *);
#endif
