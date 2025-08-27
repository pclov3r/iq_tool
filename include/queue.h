/**
 * @file queue.h
 * @brief Defines the functional interface for a thread-safe, blocking queue.
 *
 * This module provides the functions to initialize, destroy, enqueue, and
 * dequeue items from the `Queue` structure defined in `pipeline_types.h`.
 * It is the primary mechanism for passing data between threads in the
 * processing pipeline.
 */

#ifndef QUEUE_H_
#define QUEUE_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include "pipeline_types.h" // Provides the full definition for the Queue struct
#include "memory_arena.h"   // Provides the full definition for the MemoryArena struct

// --- Function Declarations ---

/**
 * @brief Initializes a thread-safe queue structure.
 *
 * This function takes a pointer to a pre-allocated Queue struct and initializes it.
 * It allocates the internal buffer for storing items from the provided memory arena.
 *
 * @param queue Pointer to the Queue struct to initialize.
 * @param capacity The maximum number of items the queue can hold.
 * @param arena Pointer to the memory arena from which to allocate the internal buffer.
 * @return true on success, false on failure.
 */
bool queue_init(Queue* queue, size_t capacity, MemoryArena* arena);

/**
 * @brief Destroys the synchronization primitives of a queue.
 *
 * Note: This does not free the queue struct or its buffer, as that memory
 * is managed by the memory arena from which it was allocated.
 *
 * @param queue Pointer to the queue to destroy.
 */
void queue_destroy(Queue* queue);

/**
 * @brief Enqueues an item into the queue.
 *
 * This is a blocking call. If the queue is full, the calling thread will sleep
 * until space becomes available or a shutdown is signaled.
 *
 * @param queue Pointer to the queue.
 * @param item The void pointer item to add to the queue.
 * @return true on success, false if a shutdown was signaled while waiting.
 */
bool queue_enqueue(Queue* queue, void* item);

/**
 * @brief Dequeues an item from the queue.
 *
 * This is a blocking call. If the queue is empty, the calling thread will sleep
 * until an item becomes available or a shutdown is signaled.
 *
 * @param queue Pointer to the queue.
 * @return The void pointer item from the queue, or NULL if a shutdown was signaled.
 */
void* queue_dequeue(Queue* queue);

/**
 * @brief Attempts to dequeue an item from the queue without blocking.
 * @param queue Pointer to the queue.
 * @return The void pointer item from the queue, or NULL if the queue is empty.
 */
void* queue_try_dequeue(Queue* queue);

/**
 * @brief Signals all threads waiting on the queue to wake up for a shutdown.
 *
 * This sets an internal flag and broadcasts to all condition variables, ensuring
 * that no threads remain blocked on `queue_enqueue` or `queue_dequeue`.
 *
 * @param queue Pointer to the queue to signal.
 */
void queue_signal_shutdown(Queue* queue);


#endif // QUEUE_H_
