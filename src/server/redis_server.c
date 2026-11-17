#include "server/tcp_server.h"
#include "server/redis_http.h"
#include <stdio.h>
int redis_server_run_config(uint16_t port, const hash_table_config *config)
{
    store_t *store = NULL;
    int result;
    tcp_application application = {&redis_http_handler, NULL, "Mini Redis"};
    if (store_create_config(config, &store) != STORE_OK) {
        fprintf(stderr, "Store initialization failed\n");
        return 1;
    }
    application.context = store;
    result = tcp_server_run_application(port, &application);
    store_destroy(store);
    return result;
}

int tcp_server_run(uint16_t port)
{
    const hash_table_config config = {1024, HASH_TABLE_LIST, NULL};
    return redis_server_run_config(port, &config);
}
