#ifndef MINI_REDIS_ADAPTER_H
#define MINI_REDIS_ADAPTER_H
#include "server/http_api.h"
#include "store.h"
extern const http_handler redis_http_handler;
int redis_server_run_config(uint16_t port, const hash_table_config *config);
int tcp_server_run(uint16_t port);
#endif
