#ifndef QUEUE_H_
#define QUEUE_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h> // For size_t

// --- Opaque Structure Definition ---
typedef struct ThreadSafeQueue Queue;


// --- Function Declarations ---

Queue* queue_create(size_t capacity);
void queue_destroy(Queue* queue);
bool queue_enqueue(Queue* queue, void* item);
void* queue_dequeue(Queue* queue);
void queue_signal_shutdown(Queue* queue);

/**
 * @brief Attempts to dequeue an item from the queue without blocking.
 * @param queue Pointer to the queue. Must not be NULL.
 * @return The dequeued item (void pointer), or NULL if the queue is empty.
 */
void* queue_try_dequeue(Queue* queue);


#endif // QUEUE_H_
