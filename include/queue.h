#ifndef QUEUE_H_
#define QUEUE_H_

#include "types.h" // <-- MODIFIED: Include types.h to get all definitions
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

// --- MODIFIED: All type definitions and forward declarations are REMOVED from this file. ---
// They now live exclusively in types.h.

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
 * Note: This no longer frees the queue struct or its buffer, as that memory
 * is managed by the memory arena.
 */
void queue_destroy(Queue* queue);

bool queue_enqueue(Queue* queue, void* item);
void* queue_dequeue(Queue* queue);
void queue_signal_shutdown(Queue* queue);
void* queue_try_dequeue(Queue* queue);


#endif // QUEUE_H_
