#include "server/tcp_server.h"
#include "server/redis_http.h"
#include <stdio.h>
static int begin_service(void *context)
{
    return persistence_begin(context, tcp_server_now_ms);
}

int redis_server_run_managed(uint16_t port, const hash_table_config *config,
                             const persistence_options *options)
{
    persistence *state = persistence_load(options, config);
    if (!state) {
        fprintf(stderr, "Store initialization failed: incompatible, unavailable or unclean persistence directory, or store allocation failed\n");
        return 1;
    }
    tcp_application application = {&redis_managed_http_handler, state, "Mini Redis"};
    int result = tcp_server_run_managed(port, &application, begin_service);
    double checkpoint_ms;
    if (result == 0 && !persistence_finish(state, tcp_server_now_ms(), &checkpoint_ms)) {
        fprintf(stderr, "Persistence failed; server stopped without a clean checkpoint\n");
        result = 1;
    }
    persistence_destroy(state);
    return result;
}

int redis_server_run_config(uint16_t port, const hash_table_config *config)
{
    const persistence_options options = {PERSISTENCE_NONE, NULL, 0};
    return redis_server_run_managed(port, config, &options);
}

int tcp_server_run(uint16_t port)
{
    const hash_table_config config = {1024, HASH_TABLE_LIST, NULL};
    return redis_server_run_config(port, &config);
}
