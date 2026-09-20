/**
 * @file networking.h
 * @brief Defines a generic, passive, blocking networking library interface.
 */

#ifndef NETWORKING_H_
#define NETWORKING_H_

#include <stdbool.h>
#include <stddef.h>

struct MemoryArena;

/**
 * @enum NetworkingState
 * @brief Lifecycle states for an active network connection handle.
 */
typedef enum NetworkingState {
  NETWORKING_STATE_UNINITIALIZED = 0,
  NETWORKING_STATE_CONNECTING, ///< Socket created, connection in progress.
  NETWORKING_STATE_CONNECTED,  ///< Socket connected and ready for I/O.
  NETWORKING_STATE_CLOSING,    ///< Shutdown/disconnect in progress.
  NETWORKING_STATE_CLOSED,     ///< Socket closed and inert.
  NETWORKING_STATE_ERROR       ///< Fatal socket error encountered.
} NetworkingState;

/**
 * @brief An opaque handle representing an active network connection.
 */
typedef struct NetworkingContext NetworkingContext;

/**
 * @brief Initializes the networking subsystem (e.g., WSAStartup on Windows).
 */
bool networking_init(void);

/**
 * @brief Cleans up the networking subsystem.
 */
void networking_cleanup(void);

/**
 * @brief Connects to a remote host and returns a handle.
 * This function allocates the NetworkingContext from the provided memory arena.
 * @param hostname The hostname or IP address of the server.
 * @param port The port number.
 * @param arena The memory arena to use for allocating the context handle.
 * @return A valid NetworkingContext handle on success, or NULL on failure.
 */
NetworkingContext *networking_connect(const char *hostname, int port,
                                      struct MemoryArena *arena);

/**
 * @brief Disconnects a network connection and releases its subsystem reference.
 * This function closes the socket and decrements the subsystem reference count.
 * It does NOT free the context handle, as its memory is managed by the memory
 * arena.
 * @param context The context handle to disconnect.
 */
void networking_disconnect(NetworkingContext *context);

/**
 * @brief Returns the current lifecycle state of the network connection.
 * @param context The context handle to inspect.
 * @return The current NetworkingState.
 */
NetworkingState networking_get_state(const NetworkingContext *context);

/**
 * @brief Reliably sends a block of bytes over the connection.
 */
bool networking_send_all(NetworkingContext *context, const void *data,
                         size_t length);

/**
 * @brief Reliably receives a block of bytes from the connection.
 */
bool networking_recv_all(NetworkingContext *context, void *data, size_t length);

#endif // NETWORKING_H_
