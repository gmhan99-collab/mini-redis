#ifndef MINI_REDIS_TCP_SERVER_H
#define MINI_REDIS_TCP_SERVER_H

#if defined(_WIN32) && !defined(FD_SETSIZE)
#define FD_SETSIZE 256
#elif defined(_WIN32) && FD_SETSIZE != 256
#error FD_SETSIZE must be 256 in every Winsock translation unit
#endif

#include <stdint.h>

#include "server/http_api.h"
typedef struct {
    const http_handler *handler;
    void *context;
    const char *name;
} tcp_application;
/* Owns Winsock and connections; application resources belong to caller. */
int tcp_server_run_application(uint16_t port, const tcp_application *application);
int tcp_server_run_managed(uint16_t port, const tcp_application *application,
                           int (*ready)(void *));
uint64_t tcp_server_now_ms(void);

#endif
