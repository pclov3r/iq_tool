/**
 * @file networking.c
 * @brief Implements the generic, passive, blocking networking library.
 *
 * This module encapsulates all platform-specific socket logic. Each connection
 * handle manages its own socket lifecycle, and all memory for connection
 * contexts is allocated from a user-provided memory arena.
 */

#include "networking.h"
#include "config/constants.h" // Added for NETWORK_SOCKET_TIMEOUT_MS
#include "log.h"
#include "mem_arena.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h> // Added for struct timeval
#include <unistd.h>
#endif

#include "platform.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// --- Private State ---

// The private, internal definition of our opaque handle.
struct NetworkingContext {
#ifdef _WIN32
  atomic_uintptr_t socket_fd;
#else
  atomic_int socket_fd;
#endif
  atomic_int state;
};

// --- Public API Implementation ---

NetworkingState networking_get_state(const NetworkingContext *context) {
  if (!context)
    return NETWORKING_STATE_UNINITIALIZED;
  return (NetworkingState)atomic_load_explicit(&context->state,
                                               memory_order_acquire);
}

NetworkingContext *networking_connect(const char *hostname, int port,
                                      struct MemoryArena *arena) {
  if (!arena) {
    log_fatal("networking_connect called with a NULL memory arena.");
    return NULL;
  }

#ifdef _WIN32
  WSADATA wsa_data;
  int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (wsa_result != 0) {
    log_fatal("WSAStartup failed with error: %d", wsa_result);
    return NULL;
  }
#endif

  struct addrinfo hints;
  struct addrinfo *address_list = NULL;
  struct addrinfo *address_node = NULL;
  int status;
  char port_string[6];
  snprintf(port_string, sizeof(port_string), "%d", port);

  log_info("Opening network connection to %s:%d...", hostname, port);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if ((status = getaddrinfo(hostname, port_string, &hints, &address_list)) !=
      0) {
    log_error("getaddrinfo for '%s' failed: %s", hostname,
              gai_strerror(status));
#ifdef _WIN32
    WSACleanup();
#endif
    return NULL;
  }

  NetworkingContext *context = (NetworkingContext *)mem_arena_alloc(
      arena, sizeof(NetworkingContext), true);
  if (!context) {
    // mem_arena_alloc already logged the fatal error.
    freeaddrinfo(address_list);
#ifdef _WIN32
    WSACleanup();
#endif
    return NULL;
  }

  atomic_store_explicit(&context->state, NETWORKING_STATE_CONNECTING,
                        memory_order_release);
#ifdef _WIN32
  atomic_store_explicit(&context->socket_fd, (uintptr_t)INVALID_SOCKET,
                        memory_order_release);
#else
  atomic_store_explicit(&context->socket_fd, -1, memory_order_release);
#endif

  for (address_node = address_list; address_node != NULL;
       address_node = address_node->ai_next) {
#ifdef _WIN32
    SOCKET socket_handle = socket(
        address_node->ai_family, address_node->ai_socktype,
        address_node->ai_protocol);
    if (socket_handle == INVALID_SOCKET)
      continue;

    // --- Apply Timeouts (Windows) ---
    // Windows setsockopt takes DWORD in milliseconds.
    DWORD timeout = NETWORK_SOCKET_TIMEOUT_MS;
    if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
      log_warn("Failed to set socket receive timeout: error %d",
               WSAGetLastError());
    }
    if (setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
      log_warn("Failed to set socket send timeout: error %d",
               WSAGetLastError());
    }
    int receive_buffer_size = 2 * 1024 * 1024;
    if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF,
                   (const char *)&receive_buffer_size,
                   sizeof(receive_buffer_size)) == SOCKET_ERROR) {
      log_warn("Failed to set socket receive buffer size: error %d",
               WSAGetLastError());
    }

    if (connect(socket_handle, address_node->ai_addr,
                (int)address_node->ai_addrlen) == SOCKET_ERROR) {
      closesocket(socket_handle);
      continue;
    }
    atomic_store_explicit(&context->socket_fd, (uintptr_t)socket_handle,
                          memory_order_release);
    break; // Successfully connected
#else
    int socket_handle = socket(address_node->ai_family,
                               address_node->ai_socktype,
                               address_node->ai_protocol);
    if (socket_handle < 0)
      continue;

    // --- Apply Timeouts (POSIX) ---
    // POSIX setsockopt takes struct timeval (seconds + microseconds).
    struct timeval timeout;
    timeout.tv_sec = NETWORK_SOCKET_TIMEOUT_MS / 1000;
    timeout.tv_usec = (NETWORK_SOCKET_TIMEOUT_MS % 1000) * 1000;
    char error_buffer[128];
    if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
      log_warn("Failed to set socket receive timeout: %s",
               platform_strerror(errno, error_buffer, sizeof(error_buffer)));
    }
    if (setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
      log_warn("Failed to set socket send timeout: %s",
               platform_strerror(errno, error_buffer, sizeof(error_buffer)));
    }
    int receive_buffer_size = 2 * 1024 * 1024;
    if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF,
                   (const char *)&receive_buffer_size,
                   sizeof(receive_buffer_size)) < 0) {
      log_warn("Failed to set socket receive buffer size: %s",
               platform_strerror(errno, error_buffer, sizeof(error_buffer)));
    }

    if (connect(socket_handle, address_node->ai_addr,
                address_node->ai_addrlen) < 0) {
      close(socket_handle);
      continue;
    }
    atomic_store_explicit(&context->socket_fd, socket_handle,
                          memory_order_release);
    break; // Successfully connected
#endif
  }

  if (address_node == NULL) {
    log_error("Failed to connect to %s:%d", hostname, port);
    atomic_store_explicit(&context->state, NETWORKING_STATE_ERROR,
                          memory_order_release);
    freeaddrinfo(address_list);
#ifdef _WIN32
    WSACleanup();
#endif
    return NULL;
  }

  freeaddrinfo(address_list);
  atomic_store_explicit(&context->state, NETWORKING_STATE_CONNECTED,
                        memory_order_release);
  log_info("Network connection established.");
  return context;
}

void networking_disconnect(NetworkingContext *context) {
  if (!context)
    return;

  NetworkingState expected = (NetworkingState)atomic_load_explicit(
      &context->state, memory_order_acquire);
  if (expected == NETWORKING_STATE_UNINITIALIZED ||
      expected == NETWORKING_STATE_CLOSING ||
      expected == NETWORKING_STATE_CLOSED) {
    return;
  }

  while (!atomic_compare_exchange_weak_explicit(
      &context->state, (int *)&expected, (int)NETWORKING_STATE_CLOSING,
      memory_order_acq_rel, memory_order_acquire)) {
    if (expected == NETWORKING_STATE_UNINITIALIZED ||
        expected == NETWORKING_STATE_CLOSING ||
        expected == NETWORKING_STATE_CLOSED) {
      return;
    }
  }

  log_info("Closing network connection...");
#ifdef _WIN32
  SOCKET socket_handle = (SOCKET)atomic_exchange_explicit(
      &context->socket_fd, (uintptr_t)INVALID_SOCKET, memory_order_acq_rel);
  if (socket_handle != INVALID_SOCKET) {
    shutdown(socket_handle, SD_BOTH);
    closesocket(socket_handle);
    WSACleanup();
  }
#else
  int socket_handle =
      atomic_exchange_explicit(&context->socket_fd, -1, memory_order_acq_rel);
  if (socket_handle >= 0) {
    shutdown(socket_handle, SHUT_RDWR);
    close(socket_handle);
  }
#endif

  atomic_store_explicit(&context->state, NETWORKING_STATE_CLOSED,
                        memory_order_release);
}

bool networking_send_all(NetworkingContext *context, const void *data,
                         size_t length) {
  if (!context || !data)
    return false;

  size_t total_sent = 0;
  while (total_sent < length) {
    if (networking_get_state(context) != NETWORKING_STATE_CONNECTED) {
      return false;
    }

    size_t to_send = length - total_sent;
    if (to_send > NETWORK_MAX_CHUNK_BYTES)
      to_send = NETWORK_MAX_CHUNK_BYTES;

#ifdef _WIN32
    SOCKET socket_handle =
        (SOCKET)atomic_load_explicit(&context->socket_fd, memory_order_acquire);
    if (socket_handle == INVALID_SOCKET)
      return false;
#else
    int socket_handle =
        atomic_load_explicit(&context->socket_fd, memory_order_acquire);
    if (socket_handle < 0)
      return false;
#endif

    int bytes_sent =
        send(socket_handle, (const char *)data + total_sent, (int)to_send, 0);
    if (bytes_sent <= 0) {
      NetworkingState current = networking_get_state(context);
      if (current != NETWORKING_STATE_CLOSING &&
          current != NETWORKING_STATE_CLOSED) {
        atomic_store_explicit(&context->state, NETWORKING_STATE_ERROR,
                              memory_order_release);
#ifdef _WIN32
        if (bytes_sent < 0 && WSAGetLastError() == WSAETIMEDOUT) {
          log_error("Network send timed out.");
        } else {
          log_error("Failed to send data to remote host.");
        }
#else
        if (bytes_sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          log_error("Network send timed out.");
        } else {
          char error_buffer[128];
          log_error("Failed to send data to remote host: %s",
                    platform_strerror(errno, error_buffer,
                                      sizeof(error_buffer)));
        }
#endif
      }
      return false;
    }
    total_sent += (size_t)bytes_sent;
  }
  return true;
}

bool networking_recv_all(NetworkingContext *context, void *data,
                         size_t length) {
  if (!context || !data)
    return false;

  size_t total_received = 0;
  while (total_received < length) {
    if (networking_get_state(context) != NETWORKING_STATE_CONNECTED) {
      return false;
    }

    size_t to_receive = length - total_received;
    if (to_receive > NETWORK_MAX_CHUNK_BYTES)
      to_receive = NETWORK_MAX_CHUNK_BYTES;

#ifdef _WIN32
    SOCKET socket_handle =
        (SOCKET)atomic_load_explicit(&context->socket_fd, memory_order_acquire);
    if (socket_handle == INVALID_SOCKET)
      return false;
#else
    int socket_handle =
        atomic_load_explicit(&context->socket_fd, memory_order_acquire);
    if (socket_handle < 0)
      return false;
#endif

    int bytes_received =
        recv(socket_handle, (char *)data + total_received, (int)to_receive, 0);
    if (bytes_received <= 0) {
      NetworkingState current = networking_get_state(context);
      if (current != NETWORKING_STATE_CLOSING &&
          current != NETWORKING_STATE_CLOSED) {
        atomic_store_explicit(&context->state, NETWORKING_STATE_ERROR,
                              memory_order_release);

#ifdef _WIN32
        if (bytes_received == 0) {
          log_error("Remote host closed the network connection.");
        } else if (WSAGetLastError() == WSAETIMEDOUT) {
          log_error("Network receive timed out.");
        } else {
          log_error("Failed to receive data from remote host.");
        }
#else
        if (bytes_received == 0) {
          log_error("Remote host closed the network connection.");
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
          log_error("Network receive timed out.");
        } else {
          char error_buffer[128];
          log_error(
              "Failed to receive data from remote host: %s",
              platform_strerror(errno, error_buffer, sizeof(error_buffer)));
        }
#endif
      }
      return false;
    }
    total_received += (size_t)bytes_received;
  }
  return true;
}
