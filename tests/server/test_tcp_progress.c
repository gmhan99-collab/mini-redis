/* Deliberately inject socket outcomes: OS packet splitting is not a test oracle. */
#include "server/tcp_server.h"
#include <winsock2.h>
#include <windows.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int send_step;
static int send_mode;
static size_t send_total;
static const unsigned char *send_base;
static int recv_mode;
static int recv_step;
static size_t recv_total;
static int fake_send(SOCKET socket, const char *data, int length, int flags)
{
    (void)socket;
    (void)flags;
    assert((const unsigned char *)data == send_base + send_total);
    assert(length > 0 && length <= 65536);
    send_step++;
    if (send_mode == 1 && send_step == 2) {
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }
    if (send_mode == 2) {
        WSASetLastError(WSAECONNRESET);
        return SOCKET_ERROR;
    }
    if (send_mode == 3) {
        return 0;
    }
    if (send_mode == 1 && send_step == 1 && length > 3) {
        length = 3;
    }
    send_total += (size_t)length;
    return length;
}

static int fake_recv(SOCKET socket, char *data, int length, int flags)
{
    (void)socket;
    (void)flags;
    recv_step++;
    if (recv_mode == 1) {
        memset(data, 'x', (size_t)length);
        recv_total += (size_t)length;
        return length;
    }
    if (recv_mode == 2 && recv_step == 1) {
        memcpy(data, "!!", 2);
        return 2;
    }
    if (recv_mode == 2) {
        return 0;
    }
    WSASetLastError(WSAEWOULDBLOCK);
    return SOCKET_ERROR;
}

static uint64_t fake_now;
static unsigned int execution_delay_ms;
static uint64_t fake_clock(void) { return fake_now; }
#define GetTickCount64 fake_clock
#define send fake_send
#define recv fake_recv
#include "../../src/server/tcp_server.c"
#undef GetTickCount64
#undef send
#undef recv

struct http_connection { int paused; };
static int executed;
static int resets;
static size_t response_size = 10;
static int feed_mode;

http_connection_t *http_connection_create(void)
{
    return calloc(1, sizeof(http_connection_t));
}
void http_connection_destroy(http_connection_t *connection)
{
    free(connection);
}
void http_connection_reset(http_connection_t *connection)
{
    connection->paused = 0;
    resets++;
}
int http_connection_feed(http_connection_t *connection, const void *data, size_t length,
                         size_t *consumed, const http_handler *handler, void *context, uint64_t now,
                         http_response *response)
{
    const unsigned char *end = memchr(data, '!', length);
    (void)handler;
    (void)context;
    (void)now;
    assert(!connection->paused);
    if (feed_mode) {
        *consumed = feed_mode == 1 ? 0 : length + 1;
        return 0;
    }
    *consumed = end ? (size_t)(end - (const unsigned char *)data) + 1 : length;
    if (!end) {
        return 0;
    }
    executed++;
    fake_now += execution_delay_ms;
    connection->paused = 1;
    response->data = malloc(response_size);
    assert(response->data);
    memset(response->data, 'r', response_size);
    response->length = response_size;
    response->close_after = 0;
    return 1;
}
static void prepare(client_connection *client, const char *input)
{
    memset(client, 0, sizeof(*client));
    client->http = http_connection_create();
    client->input_length = strlen(input);
    memcpy(client->input, input, client->input_length);
    client->idle_started = 100;
    send_step = 0;
    send_mode = 0;
    send_total = 0;
    recv_step = 0;
    recv_total = 0;
    executed = 0;
    fake_now = 0;
    execution_delay_ms = 0;
    resets = 0;
    response_size = 10;
    feed_mode = 0;
}
static void release(client_connection *client)
{
    free(client->output.data);
    http_connection_destroy(client->http);
}

static void test_partial_send_and_pipeline(void)
{
    client_connection client;
    prepare(&client, "!!");
    assert(execute_request(&client, NULL, 100));
    assert(executed == 1 && client.input_length == 1);
    send_base = client.output.data;
    send_mode = 1;
    assert(send_response(&client, 101));
    assert(client.sent == 3 && resets == 0 && client.output.data == send_base);
    assert(execute_request(&client, NULL, 102));
    assert(executed == 1);
    assert(send_response(&client, 103));
    assert(client.output.data == NULL && resets == 1 && send_total == 10);
    /* No recv call or readability event between responses. */
    assert(execute_request(&client, NULL, 104));
    assert(executed == 2 && client.input_length == 0);
    release(&client);
}

static void test_write_budget(void)
{
    client_connection client;
    prepare(&client, "!");
    response_size = IO_BUDGET + 23;
    assert(execute_request(&client, NULL, 100));
    send_base = client.output.data;
    assert(send_response(&client, 101));
    assert(client.sent == IO_BUDGET && client.output.data != NULL);
    assert(send_response(&client, 102));
    assert(send_total == IO_BUDGET + 23 && client.output.data == NULL);
    release(&client);
}

static void test_read_budget_and_backpressure(void)
{
    client_connection client;
    prepare(&client, "");
    recv_mode = 1;
    assert(receive_client(&client, 100));
    assert(client.input_length == INPUT_CAPACITY && recv_total == IO_BUDGET);
    assert(client.receiving && client.receive_started == 100);
    assert(receive_client(&client, 100));
    assert(recv_total == IO_BUDGET);
    release(&client);
}

static void test_eof_drains_complete_requests(void)
{
    client_connection client;
    prepare(&client, "");
    recv_mode = 2;
    assert(receive_client(&client, 100));
    assert(client.eof && client.input_length == 2);
    assert(execute_request(&client, NULL, 100));
    send_base = client.output.data;
    assert(send_response(&client, 101));
    assert(execute_request(&client, NULL, 102));
    send_base = client.output.data;
    send_total = 0;
    assert(!send_response(&client, 103));
    assert(executed == 2 && resets == 2);
    release(&client);
}

static void test_incomplete_eof_and_close(void)
{
    client_connection client;
    prepare(&client, "unfinished");
    client.eof = 1;
    assert(!execute_request(&client, NULL, 100));
    assert(executed == 0);
    release(&client);
    prepare(&client, "!!");
    assert(execute_request(&client, NULL, 100));
    client.output.close_after = 1;
    send_base = client.output.data;
    assert(send_response(&client, 101));
    assert(client.closing && client.input_length == 0);
    assert(execute_request(&client, NULL, 102) && executed == 1);
    assert(client_wait(&client, 101 + TCP_CLOSE_TIMEOUT_MS) == 0);
    recv_mode = 2;
    recv_step = 0;
    assert(!receive_client(&client, 103));
    assert(executed == 1);
    release(&client);
}

static void test_absolute_deadlines(void)
{
    client_connection client;
    prepare(&client, "part");
    assert(client_wait(&client, 100 + TCP_IDLE_TIMEOUT_MS) == 0);
    assert(execute_request(&client, NULL, 200));
    memcpy(client.input, "more", 4);
    client.input_length = 4;
    assert(execute_request(&client, NULL, 300));
    assert(client.receive_started == 200);
    assert(client_wait(&client, 200 + TCP_RECEIVE_TIMEOUT_MS) == 0);
    memcpy(client.input, "!", 1);
    client.input_length = 1;
    assert(execute_request(&client, NULL, 400));
    send_base = client.output.data;
    send_mode = 1;
    assert(send_response(&client, 500));
    assert(client.send_started == 400);
    assert(client_wait(&client, 400 + TCP_SEND_TIMEOUT_MS) == 0);
    release(&client);
}

static void test_send_deadline_starts_after_handler(void)
{
    client_connection client;
    prepare(&client, "!");
    execution_delay_ms = 40000;
    assert(execute_request(&client, NULL, 100));
    assert(client.send_started == 40100);
    assert(client_wait(&client, 40100) == TCP_SEND_TIMEOUT_MS);
    assert(client_wait(&client, 40100 + TCP_SEND_TIMEOUT_MS) == 0);
    release(&client);
}

static void test_socket_errors(void)
{
    client_connection client;
    int mode;
    for (mode = 2; mode <= 3; mode++) {
        prepare(&client, "!");
        assert(execute_request(&client, NULL, 100));
        send_base = client.output.data;
        send_mode = mode;
        assert(!send_response(&client, 101));
        assert(client.sent == 0 && resets == 0);
        release(&client);
    }
    prepare(&client, "");
    recv_mode = 0;
    assert(receive_client(&client, 100));
    assert(client.input_length == 0 && !client.eof);
    release(&client);
}

static void test_interest_and_internal_work(void)
{
    client_connection client;
    fd_set reads;
    fd_set writes;
    prepare(&client, "");
    client.socket = 123;
    FD_ZERO(&reads);
    FD_ZERO(&writes);
    assert(!watch_client(&client, &reads, &writes));
    assert(FD_ISSET(client.socket, &reads));
    assert(!FD_ISSET(client.socket, &writes));
    client.input_length = INPUT_CAPACITY;
    FD_ZERO(&reads);
    assert(watch_client(&client, &reads, &writes));
    assert(!FD_ISSET(client.socket, &reads));
    client.output.data = malloc(1);
    assert(!watch_client(&client, &reads, &writes));
    assert(FD_ISSET(client.socket, &writes));
    free(client.output.data);
    client.output.data = NULL;
    client.input_length = 0;
    client.eof = 1;
    assert(watch_client(&client, &reads, &writes));
    release(&client);
}

static void test_parser_progress_guard(void)
{
    client_connection client;
    int mode;
    for (mode = 1; mode <= 2; mode++) {
        prepare(&client, "part");
        feed_mode = mode;
        assert(!execute_request(&client, NULL, 100));
        assert(executed == 0);
        release(&client);
    }
}

static void test_accept_budget_and_nonblocking(void)
{
    client_connection *clients[MAX_CONNECTIONS] = {0};
    SOCKET peers[ACCEPT_BUDGET + 4];
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address;
    int address_length = sizeof(address);
    size_t i;
    size_t accepted = 0;
    assert(listener != INVALID_SOCKET);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_length) == 0);
    assert(listen(listener, SOMAXCONN) == 0);
    assert(set_nonblocking(listener));
    for (i = 0; i < ACCEPT_BUDGET + 4; i++) {
        peers[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(peers[i] != INVALID_SOCKET);
        assert(connect(peers[i], (struct sockaddr *)&address, sizeof(address)) == 0);
    }
    accept_clients(listener, clients, 100);
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        if (clients[i] != NULL) {
            char byte;
            accepted++;
            /* A real accepted socket must return immediately with no input. */
            assert(recv(clients[i]->socket, &byte, 1, 0) == SOCKET_ERROR);
            assert(WSAGetLastError() == WSAEWOULDBLOCK);
        }
    }
    assert(accepted == ACCEPT_BUDGET);
    accept_clients(listener, clients, 101);
    accepted = 0;
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        if (clients[i] != NULL) {
            accepted++;
            destroy_client(clients[i]);
        }
    }
    assert(accepted == ACCEPT_BUDGET + 4);
    for (i = 0; i < ACCEPT_BUDGET + 4; i++) {
        closesocket(peers[i]);
    }
    closesocket(listener);
}

static void test_shutdown_notification(void)
{
    InterlockedExchange(&stop_requested, 0);
    assert(!request_shutdown(CTRL_LOGOFF_EVENT));
    assert(InterlockedCompareExchange(&stop_requested, 0, 0) == 0);
    assert(request_shutdown(CTRL_C_EVENT));
    assert(InterlockedCompareExchange(&stop_requested, 0, 0) == 1);
    InterlockedExchange(&stop_requested, 0);
    assert(request_shutdown(CTRL_BREAK_EVENT));
    assert(InterlockedCompareExchange(&stop_requested, 0, 0) == 1);
    InterlockedExchange(&stop_requested, 0);
}

int main(void)
{
    WSADATA winsock;
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    test_partial_send_and_pipeline();
    test_write_budget();
    test_read_budget_and_backpressure();
    test_eof_drains_complete_requests();
    test_incomplete_eof_and_close();
    test_absolute_deadlines();
    test_send_deadline_starts_after_handler();
    test_socket_errors();
    test_interest_and_internal_work();
    test_parser_progress_guard();
    test_accept_budget_and_nonblocking();
    test_shutdown_notification();
    WSACleanup();
    puts("TCP progress: 12/12 passed");
    return 0;
}
