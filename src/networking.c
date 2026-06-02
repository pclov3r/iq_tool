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
#include "constants.h" // Added for NETWORK_SOCKET_TIMEOUT_MS
#include "log.h"
#include "mem_arena.h"
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
#include <sys/time.h> // Added for struct timeval
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

bool networking_init(void) {
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

void networking_cleanup(void) {
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
    if (!networking_init()) {
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
        networking_cleanup(); // Decrement ref count on failure.
        return NULL;
    }

    NetworkingContext* context = (NetworkingContext*)mem_arena_alloc(arena, sizeof(NetworkingContext), true);
    if (!context) {
        // mem_arena_alloc already logged the fatal error.
        freeaddrinfo(res);
        networking_cleanup(); // Decrement ref count on failure.
        return NULL;
    }

#ifdef _WIN32
    context->socket_fd = INVALID_SOCKET;
#else
    context->socket_fd = -1;
#endif

    for (p = res; p != NULL; p = p->ai_next) {
        context->socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
#ifdef _WIN32
        if (context->socket_fd == INVALID_SOCKET) continue;

        // --- Apply Timeouts (Windows) ---
        // Windows setsockopt takes DWORD in milliseconds.
        DWORD timeout = NETWORK_SOCKET_TIMEOUT_MS;
        setsockopt(context->socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(context->socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(context->socket_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

        if (connect(context->socket_fd, p->ai_addr, (int)p->ai_addrlen) == SOCKET_ERROR) {
            closesocket(context->socket_fd);
            context->socket_fd = INVALID_SOCKET;
            continue;
        }
#else
        if (context->socket_fd < 0) continue;

        // --- Apply Timeouts (POSIX) ---
        // POSIX setsockopt takes struct timeval (seconds + microseconds).
        struct timeval timeout;
        timeout.tv_sec = NETWORK_SOCKET_TIMEOUT_MS / 1000;
        timeout.tv_usec = (NETWORK_SOCKET_TIMEOUT_MS % 1000) * 1000;
        setsockopt(context->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(context->socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(context->socket_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

        if (connect(context->socket_fd, p->ai_addr, p->ai_addrlen) < 0) {
            close(context->socket_fd);
            context->socket_fd = -1;
            continue;
        }
#endif
        break; // Successfully connected
    }

    freeaddrinfo(res);

#ifdef _WIN32
    if (context->socket_fd == INVALID_SOCKET) {
#else
    if (context->socket_fd < 0) {
#endif
        log_error("Failed to connect to %s:%d", hostname, port);
        // We don't free(context) because it's in the arena. The arena will be destroyed on app cleanup.
        networking_cleanup(); // Decrement ref count on failure.
        return NULL;
    }

    return context;
}

void networking_disconnect(NetworkingContext* context) {
    if (!context) return;
#ifdef _WIN32
    if (context->socket_fd != INVALID_SOCKET) {
        shutdown(context->socket_fd, SD_BOTH);
        closesocket(context->socket_fd);
        context->socket_fd = INVALID_SOCKET; // Mark as closed
    }
#else
    if (context->socket_fd >= 0) {
        shutdown(context->socket_fd, SHUT_RDWR);
        close(context->socket_fd);
        context->socket_fd = -1; // Mark as closed
    }
#endif
    // No free(context), as the memory is managed by the arena.
}

bool networking_send_all(NetworkingContext* context, const void* data, size_t length) {
    if (!context || !data) return false;
    size_t total_sent = 0;
    while (total_sent < length) {
        int sent = send(context->socket_fd, (const char*)data + total_sent, (int)(length - total_sent), 0);
        if (sent <= 0) {
#ifdef _WIN32
            if (sent < 0 && WSAGetLastError() == WSAETIMEDOUT) {
                log_error("Network send timed out.");
            } else
#else
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                log_error("Network send timed out.");
            } else
#endif
            {
                log_error("Failed to send data to remote host.");
            }
            return false;
        }
        total_sent += sent;
    }
    return true;
}

bool networking_recv_all(NetworkingContext* context, void* data, size_t length) {
    if (!context || !data) return false;
    size_t total_recv = 0;
    while (total_recv < length) {
        int recvd = recv(context->socket_fd, (char*)data + total_recv, (int)(length - total_recv), 0);
        if (recvd <= 0) {
#ifdef _WIN32
            if (recvd < 0 && WSAGetLastError() == WSAETIMEDOUT) {
                log_error("Network receive timed out.");
            } else
#else
            if (recvd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                log_error("Network receive timed out.");
            } else
#endif
            {
                log_error("Failed to receive data from remote host (connection closed or error).");
            }
            return false;
        }
        total_recv += recvd;
    }
    return true;
}
