/**
 * @file networking.c
 * @brief Implements the generic, passive, blocking networking library.
 *
 * This module encapsulates all platform-specific socket logic. Its lifecycle
 * is managed by a reference counter, allowing it to be safely used by multiple
 * modules without redundant initialization or premature cleanup. All memory for
 * connection contexts is allocated from a user-provided memory arena.
 */

#include "networking.h"
#include "log.h"
#include "memory_arena.h"
#include <stdlib.h>
#include <string.h>

// --- Platform-Specific Networking Includes ---
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#endif

// --- Private State ---
// This reference counter makes the library's lifecycle self-managing.
static int g_networking_ref_count = 0;

// The private, internal definition of our opaque handle.
struct NetworkingContext {
#ifdef _WIN32
    SOCKET socket_fd;
#else
    int socket_fd;
#endif
};


// --- Public API Implementation ---

bool networking_initialize_module(void) {
    if (g_networking_ref_count > 0) {
        g_networking_ref_count++;
        log_debug("Networking subsystem reference count increased to %d.", g_networking_ref_count);
        return true; // Already initialized, just increment count.
    }

#ifdef _WIN32
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        log_fatal("WSAStartup failed with error: %d", result);
        return false;
    }
#endif

    log_debug("Networking subsystem initialized for the first time.");
    g_networking_ref_count = 1;
    return true;
}

void networking_cleanup_module(void) {
    if (g_networking_ref_count <= 0) {
        return; // Nothing to clean up or already cleaned up.
    }

    g_networking_ref_count--;
    log_debug("Networking subsystem reference count decreased to %d.", g_networking_ref_count);

    if (g_networking_ref_count == 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        log_debug("Networking subsystem cleaned up as last reference was released.");
    }
}

NetworkingContext* networking_connect(const char* hostname, int port, struct MemoryArena* arena) {
    if (!arena) {
        log_fatal("networking_connect called with a NULL memory arena.");
        return NULL;
    }
 
    // This function acts as the gatekeeper, ensuring the subsystem is ready.
    if (!networking_initialize_module()) {
        log_error("Cannot connect because networking subsystem failed to initialize.");
        return NULL;
    }

    struct addrinfo hints, *res, *p;
    int status;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((status = getaddrinfo(hostname, port_str, &hints, &res)) != 0) {
        log_error("getaddrinfo for '%s' failed: %s", hostname, gai_strerror(status));
        networking_cleanup_module(); // Decrement ref count on failure.
        return NULL;
    }

    NetworkingContext* ctx = (NetworkingContext*)mem_arena_alloc(arena, sizeof(NetworkingContext), true);
    if (!ctx) {
        // mem_arena_alloc already logged the fatal error.
        freeaddrinfo(res);
        networking_cleanup_module(); // Decrement ref count on failure.
        return NULL;
    }

#ifdef _WIN32
    ctx->socket_fd = INVALID_SOCKET;
#else
    ctx->socket_fd = -1;
#endif

    for (p = res; p != NULL; p = p->ai_next) {
        ctx->socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
#ifdef _WIN32
        if (ctx->socket_fd == INVALID_SOCKET) continue;
        if (connect(ctx->socket_fd, p->ai_addr, (int)p->ai_addrlen) == SOCKET_ERROR) {
            closesocket(ctx->socket_fd);
            ctx->socket_fd = INVALID_SOCKET;
            continue;
        }
#else
        if (ctx->socket_fd < 0) continue;
        if (connect(ctx->socket_fd, p->ai_addr, p->ai_addrlen) < 0) {
            close(ctx->socket_fd);
            ctx->socket_fd = -1;
            continue;
        }
#endif
        break; // Successfully connected
    }

    freeaddrinfo(res);

#ifdef _WIN32
    if (ctx->socket_fd == INVALID_SOCKET) {
#else
    if (ctx->socket_fd < 0) {
#endif
        log_error("Failed to connect to %s:%d", hostname, port);
        // We don't free(ctx) because it's in the arena. The arena will be destroyed on app cleanup.
        networking_cleanup_module(); // Decrement ref count on failure.
        return NULL;
    }

    return ctx;
}

void networking_disconnect(NetworkingContext* ctx) {
    if (!ctx) return;
#ifdef _WIN32
    if (ctx->socket_fd != INVALID_SOCKET) {
        shutdown(ctx->socket_fd, SD_BOTH);
        closesocket(ctx->socket_fd);
        ctx->socket_fd = INVALID_SOCKET; // Mark as closed
    }
#else
    if (ctx->socket_fd >= 0) {
        shutdown(ctx->socket_fd, SHUT_RDWR);
        close(ctx->socket_fd);
        ctx->socket_fd = -1; // Mark as closed
    }
#endif
    // No free(ctx), as the memory is managed by the arena.
}

bool networking_send_all(NetworkingContext* ctx, const void* data, size_t len) {
    if (!ctx || !data) return false;
    size_t total_sent = 0;
    while (total_sent < len) {
        int sent = send(ctx->socket_fd, (const char*)data + total_sent, (int)(len - total_sent), 0);
        if (sent <= 0) {
            log_error("Failed to send data to remote host.");
            return false;
        }
        total_sent += sent;
    }
    return true;
}

bool networking_recv_all(NetworkingContext* ctx, void* data, size_t len) {
    if (!ctx || !data) return false;
    size_t total_recv = 0;
    while (total_recv < len) {
        int recvd = recv(ctx->socket_fd, (char*)data + total_recv, (int)(len - total_recv), 0);
        if (recvd <= 0) {
            // recv returns 0 for a clean disconnect, which we treat as an error
            // since the caller was expecting to receive a specific number of bytes.
            log_error("Failed to receive data from remote host (connection closed or error).");
            return false;
        }
        total_recv += recvd;
    }
    return true;
}
