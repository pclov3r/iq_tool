#ifndef QUEUE_H_
#define QUEUE_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h> // For size_t

// --- Opaque Structure Definition ---
// The actual implementation is hidden in queue.c
typedef struct ThreadSafeQueue Queue;

// --- Function Declarations ---

/**
 * @brief Creates a thread-safe queue.
 * @param capacity The maximum number of items (void pointers) the queue can hold.
 * @return Pointer to the created queue, or NULL on failure (e.g., memory allocation, mutex/cond init).
 */
Queue* queue_create(size_t capacity);

/**
 * @brief Destroys a thread-safe queue.
 *        This function assumes threads are no longer using the queue.
 *        It does NOT free the items pointed to by the elements remaining in the queue.
 * @param q Pointer to the queue to destroy. If NULL, the function does nothing.
 */
void queue_destroy(Queue* q);

/**
 * @brief Enqueues an item onto the queue. Blocks if the queue is full.
 *        Handles shutdown detection.
 * @param q Pointer to the queue. Must not be NULL.
 * @param item The item (void pointer) to enqueue.
 * @return true on success, false if the queue is shutting down (detected before or during wait).
 */
bool queue_enqueue(Queue* q, void* item);

/**
 * @brief Dequeues an item from the queue. Blocks if the queue is empty.
 *        Handles shutdown detection.
 * @param q Pointer to the queue. Must not be NULL.
 * @return The dequeued item (void pointer), or NULL if the queue is shutting down
 *         (detected before or during wait) or is empty and shutting down.
 */
void* queue_dequeue(Queue* q);

/**
 * @brief Signals all threads waiting on the queue's condition variables to wake up
 *        and sets the queue's internal shutting_down flag.
 *        Used during shutdown to wake up potentially blocked threads (enqueue/dequeue).
 * @param q Pointer to the queue. Must not be NULL.
 */
void queue_signal_shutdown(Queue* q);

#endif // QUEUE_H_
