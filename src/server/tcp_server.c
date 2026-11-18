#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif
#include "server/tcp_server.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_SEND SHUT_WR
#define closesocket close
#endif
#include "server/http_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNECTIONS 128
#define ACCEPT_BUDGET 16
#define IO_BUDGET (64u * 1024u)
#define INPUT_CAPACITY (64u * 1024u)
#define TCP_CLOSE_TIMEOUT_MS 1000u
#ifndef TCP_RECEIVE_TIMEOUT_MS
#define TCP_RECEIVE_TIMEOUT_MS 10000u
#endif
#ifndef TCP_IDLE_TIMEOUT_MS
#define TCP_IDLE_TIMEOUT_MS 30000u
#endif
#ifndef TCP_SEND_TIMEOUT_MS
#define TCP_SEND_TIMEOUT_MS 10000u
#endif

typedef struct {
    SOCKET socket;
    http_connection_t *http;
    unsigned char input[INPUT_CAPACITY];
    size_t input_length;
    http_response output;
    size_t sent;
    uint64_t receive_started;
    uint64_t send_started;
    uint64_t idle_started;
    int receiving;
    int eof;
    int closing;
    uint64_t close_started;
} client_connection;

#ifdef _WIN32
static volatile LONG stop_requested;
static BOOL WINAPI request_shutdown(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

static int stopping(void)
{
    return InterlockedCompareExchange(&stop_requested, 0, 0) != 0;
}

uint64_t tcp_server_now_ms(void)
{
    return GetTickCount64();
}

static int would_block(void)
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

static int set_nonblocking(SOCKET socket)
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
#else
static volatile sig_atomic_t stop_requested;

static void request_shutdown(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int stopping(void)
{
    return stop_requested;
}

uint64_t tcp_server_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static int set_nonblocking(SOCKET socket)
{
    int flags = fcntl(socket, F_GETFL);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

static void destroy_client(client_connection *client)
{
    if (client == NULL) {
        return;
    }
    closesocket(client->socket);
    http_connection_destroy(client->http);
    free(client->output.data);
    free(client);
}

static client_connection *create_client(SOCKET socket, uint64_t now)
{
    client_connection *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        closesocket(socket);
        return NULL;
    }
    client->socket = socket;
    client->idle_started = now;
    client->http = http_connection_create();
    if (client->http == NULL || !set_nonblocking(socket)) {
        destroy_client(client);
        return NULL;
    }
    return client;
}

/* Reading while a response is pending is bounded by this staging buffer. */
static int receive_client(client_connection *client, uint64_t now)
{
    size_t remaining = IO_BUDGET;
    if (client->closing) {
        char discard[4096];
        while (!client->eof && remaining) {
            int count = recv(client->socket, discard, sizeof(discard), 0);
            if (count == 0) {
                return 0;
            }
            if (count < 0) {
                return would_block();
            }
            remaining -= (size_t)count;
        }
        return 1;
    }
    while (!client->eof && remaining && client->input_length < INPUT_CAPACITY) {
        size_t available = INPUT_CAPACITY - client->input_length;
        int received;
        if (available > remaining) {
            available = remaining;
        }
        received = recv(client->socket, (char *)client->input + client->input_length,
                        (int)available, 0);
        if (received > 0) {
            if (!client->receiving && client->output.data == NULL) {
                client->receiving = 1;
                client->receive_started = now;
            }
            client->input_length += (size_t)received;
            remaining -= (size_t)received;
        } else if (received == 0) {
            client->eof = 1;
        } else {
            return would_block();
        }
    }
    return 1;
}

/* Called once per loop: a completed response always blocks further execution. */
static int execute_request(client_connection *client, const tcp_application *application, uint64_t now)
{
    size_t consumed = 0;
    uint64_t feed_started;
    int result;
    if (client->closing) {
        return !client->eof;
    }
    if (client->output.data != NULL) {
        return 1;
    }
    if (client->input_length == 0) {
        return !client->eof;
    }
    if (!client->receiving) {
        client->receiving = 1;
        client->receive_started = now;
    }
    feed_started = tcp_server_now_ms();
    result = http_connection_feed(client->http, client->input, client->input_length,
                                  &consumed, application ? application->handler : NULL,
                                  application ? application->context : NULL, now, &client->output);
    /* An incomplete feed must consume input, otherwise internal work spins. */
    if (consumed > client->input_length || result < 0 ||
        (result == 0 && consumed == 0)) {
        return 0;
    }
    client->input_length -= consumed;
    memmove(client->input, client->input + consumed, client->input_length);
    if (result == 1) {
        if (client->output.data == NULL || client->output.length == 0) {
            return 0;
        }
        client->receiving = 0;
        client->sent = 0;
        /* SQL can block this thread for longer than a network deadline. The send
         * deadline starts when a response exists, excluding handler execution. */
        client->send_started = now + (tcp_server_now_ms() - feed_started);
        return 1;
    }
    /* EOF with an incomplete message cannot become complete later. */
    return !client->eof;
}

static int send_response(client_connection *client, uint64_t now)
{
    size_t remaining = IO_BUDGET;
    while (client->output.data != NULL && remaining) {
        size_t count = client->output.length - client->sent;
        int sent;
        if (count > remaining) {
            count = remaining;
        }
        sent = send(client->socket, (const char *)client->output.data + client->sent,
                    (int)count, 0);
        if (sent == SOCKET_ERROR) {
            return would_block();
        }
        if (sent == 0) {
            return 0;
        }
        client->sent += (size_t)sent;
        remaining -= (size_t)sent;
        if (client->sent == client->output.length) {
            int close_after = client->output.close_after;
            free(client->output.data);
            memset(&client->output, 0, sizeof(client->output));
            client->sent = 0;
            http_connection_reset(client->http);
            client->idle_started = now;
            if (close_after) {
                /* A close with unread peer bytes can discard an error response on Windows.
                 * Half-close after sending, then drain with an absolute bounded deadline. */
                shutdown(client->socket, SD_SEND);
                client->closing = 1;
                client->close_started = now;
                client->input_length = 0;
                return !client->eof;
            }
            return !(client->eof && client->input_length == 0);
        }
    }
    return 1;
}

static uint64_t time_remaining(uint64_t now, uint64_t started, uint64_t limit)
{
    uint64_t elapsed = now - started;
    return elapsed >= limit ? 0 : limit - elapsed;
}

static uint64_t client_wait(client_connection *client, uint64_t now)
{
    if (client->closing) {
        return time_remaining(now, client->close_started, TCP_CLOSE_TIMEOUT_MS);
    }
    if (client->output.data != NULL) {
        return time_remaining(now, client->send_started, TCP_SEND_TIMEOUT_MS);
    }
    if (client->receiving) {
        return time_remaining(now, client->receive_started, TCP_RECEIVE_TIMEOUT_MS);
    }
    return time_remaining(now, client->idle_started, TCP_IDLE_TIMEOUT_MS);
}

static int watch_client(client_connection *client, fd_set *reads, fd_set *writes)
{
    if (client->closing) {
        FD_SET(client->socket, reads);
        return 0;
    }
    if (!client->eof && client->input_length < INPUT_CAPACITY) {
        FD_SET(client->socket, reads);
    }
    if (client->output.data != NULL) {
        FD_SET(client->socket, writes);
        return 0;
    }
    return client->input_length != 0 || client->eof;
}

static void accept_clients(SOCKET listener, client_connection **clients, uint64_t now)
{
    unsigned int accepted;
    for (accepted = 0; accepted < ACCEPT_BUDGET; accepted++) {
        size_t slot;
        SOCKET socket = accept(listener, NULL, NULL);
        if (socket == INVALID_SOCKET) {
            return;
        }
#ifndef _WIN32
        if (socket >= FD_SETSIZE) {
            closesocket(socket);
            continue;
        }
#endif
        for (slot = 0; slot < MAX_CONNECTIONS && clients[slot] != NULL; slot++) {
        }
        if (slot == MAX_CONNECTIONS) {
            closesocket(socket);
        } else {
            clients[slot] = create_client(socket, now);
        }
    }
}

static int select_clients(SOCKET highest, fd_set *reads, fd_set *writes, struct timeval *timeout)
{
#ifdef _WIN32
    (void)highest;
    return select(0, reads, writes, NULL, timeout);
#else
    return select(highest + 1, reads, writes, NULL, timeout);
#endif
}

/* Once a handler requests shutdown, only its socket can progress. */
static void drain_shutdown_response(client_connection *client)
{
    while (client->output.data || client->closing) {
        fd_set sockets;
        uint64_t left = client_wait(client, tcp_server_now_ms());
        struct timeval timeout = {(long)(left / 1000), (long)(left % 1000 * 1000)};
        FD_ZERO(&sockets);
        FD_SET(client->socket, &sockets);
        if (!left || select_clients(client->socket, client->closing ? &sockets : NULL,
                                    client->closing ? NULL : &sockets, &timeout) <= 0) {
            break;
        }
        uint64_t now = tcp_server_now_ms();
        if (!(client->closing ? receive_client(client, now) : send_response(client, now))) {
            break;
        }
    }
}

static int run_loop(SOCKET listener, const tcp_application *application, client_connection **clients)
{
    while (!stopping()) {
        fd_set reads;
        fd_set writes;
        struct timeval timeout;
        SOCKET highest = listener;
        uint64_t now = tcp_server_now_ms();
        uint64_t wait_ms = 100;
        size_t i;
        int ready;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        for (i = 0; i < MAX_CONNECTIONS; i++) {
            client_connection *client = clients[i];
            uint64_t remaining;
            if (client == NULL) {
                continue;
            }
            remaining = client_wait(client, now);
            if (remaining == 0) {
                destroy_client(client);
                clients[i] = NULL;
                continue;
            }
            if (remaining < wait_ms) {
                wait_ms = remaining;
            }
            if (watch_client(client, &reads, &writes)) {
                wait_ms = 0;
            }
            if (client->socket > highest) {
                highest = client->socket;
            }
        }
        timeout.tv_sec = 0;
        timeout.tv_usec = (long)(wait_ms * 1000);
        ready = select_clients(highest, &reads, &writes, &timeout);
        if (ready == SOCKET_ERROR) {
#ifdef _WIN32
            fprintf(stderr, "select failed: %d\n", WSAGetLastError());
#else
            if (errno == EINTR) {
                continue;
            }
            perror("select");
#endif
            return 1;
        }
        now = tcp_server_now_ms();
        if (stopping()) {
            break;
        }
        if (FD_ISSET(listener, &reads)) {
            accept_clients(listener, clients, now);
        }
        for (i = 0; i < MAX_CONNECTIONS; i++) {
            client_connection *client = clients[i];
            int alive = 1;
            if (client == NULL) {
                continue;
            }
            if (stopping()) {
                return 0;
            }
            now = tcp_server_now_ms();
            if (client_wait(client, now) == 0) {
                alive = 0;
            }
            if (alive && FD_ISSET(client->socket, &reads)) {
                alive = receive_client(client, now);
            }
            if (alive) {
                alive = execute_request(client, application, now);
                if (client->output.stop_after) {
                    drain_shutdown_response(client);
                    return 0;
                }
            }
            if (alive && client->output.data != NULL && FD_ISSET(client->socket, &writes)) {
                alive = send_response(client, tcp_server_now_ms());
            }
            if (!alive) {
                destroy_client(client);
                clients[i] = NULL;
            }
        }
    }
    return 0;
}

int tcp_server_run_managed(uint16_t port, const tcp_application *application, int (*ready)(void *))
{
#ifdef _WIN32
    WSADATA winsock;
    int handler_registered = 0;
    const BOOL exclusive_address = TRUE;
#else
    struct sigaction handler = {0}, ignore = {0}, old_int, old_term, old_pipe;
    int int_installed = 0, term_installed = 0, pipe_installed = 0;
#endif
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    client_connection *clients[MAX_CONNECTIONS] = {0};
    int result = 1;
    size_t i;
#ifdef _WIN32
    if (port == 0 || WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        fprintf(stderr, "Winsock initialization failed or invalid port\n");
        return 1;
    }
    InterlockedExchange(&stop_requested, 0);
    handler_registered = SetConsoleCtrlHandler(request_shutdown, TRUE);
    if (!handler_registered) {
        goto initialization_error;
    }
#else
    stop_requested = 0;
    handler.sa_handler = request_shutdown;
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&handler.sa_mask);
    sigemptyset(&ignore.sa_mask);
    int_installed = sigaction(SIGINT, &handler, &old_int) == 0;
    term_installed = sigaction(SIGTERM, &handler, &old_term) == 0;
    pipe_installed = sigaction(SIGPIPE, &ignore, &old_pipe) == 0;
    if (!int_installed || !term_installed || !pipe_installed) {
        goto initialization_error;
    }
#endif
    if (!port || !application || !application->handler) {
        goto initialization_error;
    }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET || !set_nonblocking(listener)) {
        goto socket_error;
    }
#ifdef _WIN32
    /* Windows otherwise permits another socket to steal a live listening port. */
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive_address, sizeof(exclusive_address)) == SOCKET_ERROR) {
        goto socket_error;
    }
#else
    const int reuse = 1;
    if (listener >= FD_SETSIZE ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == SOCKET_ERROR) {
        goto socket_error;
    }
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        goto socket_error;
    }
    if (ready && !ready(application->context)) {
        goto initialization_error;
    }
    printf("%s listening on http://127.0.0.1:%u\n", application->name, (unsigned int)port);
    fflush(stdout);
    result = run_loop(listener, application, clients);
    goto cleanup;

socket_error:
#ifdef _WIN32
    fprintf(stderr, "Socket initialization failed: %d\n", WSAGetLastError());
#else
    perror("Socket initialization");
#endif
    goto cleanup;
initialization_error:
    fprintf(stderr, "Server initialization failed\n");
cleanup:
    if (listener != INVALID_SOCKET) {
        closesocket(listener);
    }
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        destroy_client(clients[i]);
    }
#ifdef _WIN32
    if (handler_registered) {
        SetConsoleCtrlHandler(request_shutdown, FALSE);
    }
    WSACleanup();
#else
    if (int_installed) {
        sigaction(SIGINT, &old_int, NULL);
    }
    if (term_installed) {
        sigaction(SIGTERM, &old_term, NULL);
    }
    if (pipe_installed) {
        sigaction(SIGPIPE, &old_pipe, NULL);
    }
#endif
    return result;
}

int tcp_server_run_application(uint16_t port, const tcp_application *application)
{
    return tcp_server_run_managed(port, application, NULL);
}
