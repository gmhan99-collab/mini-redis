#ifndef MINI_REDIS_TCP_SERVER_H
#define MINI_REDIS_TCP_SERVER_H

#ifndef FD_SETSIZE
#define FD_SETSIZE 256
#elif FD_SETSIZE != 256
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

#endif
