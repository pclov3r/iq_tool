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
 * @brief An opaque handle representing an active network connection.
 */
typedef struct NetworkingContext NetworkingContext;

/**
 * @brief Initializes the networking subsystem (e.g., WSAStartup on Windows).
 */
bool networking_initialize_module(void);

/**
 * @brief Cleans up the networking subsystem.
 */
void networking_cleanup_module(void);

/**
 * @brief Connects to a remote host and returns a handle.
 * This function allocates the NetworkingContext from the provided memory arena.
 * @param hostname The hostname or IP address of the server.
 * @param port The port number.
 * @param arena The memory arena to use for allocating the context handle.
 * @return A valid NetworkingContext handle on success, or NULL on failure.
 */
NetworkingContext* networking_connect(const char* hostname, int port, struct MemoryArena* arena);

/**
 * @brief Disconnects a network connection.
 * This function ONLY closes the socket. It does NOT free the context handle,
 * as its memory is managed by the memory arena.
 * @param ctx The context handle to disconnect.
 */
void networking_disconnect(NetworkingContext* ctx);

/**
 * @brief Reliably sends a block of bytes over the connection.
 */
bool networking_send_all(NetworkingContext* ctx, const void* data, size_t len);

/**
 * @brief Reliably receives a block of bytes from the connection.
 */
bool networking_recv_all(NetworkingContext* ctx, void* data, size_t len);

#endif // NETWORKING_H_
