#include "queue.h"
#include "log.h" // MODIFIED: Include the logging library header
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

/**
 * @brief A standard, blocking, thread-safe queue implementation.
 */
struct ThreadSafeQueue {
    void** buffer;
    size_t capacity;
    size_t count;
    size_t head;
    size_t tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty_cond;
    pthread_cond_t not_full_cond;
    bool shutting_down;
};

/**
 * @brief Creates a thread-safe queue.
 */
Queue* queue_create(size_t capacity) {
    if (capacity == 0) {
        // MODIFIED: Use the logging library
        log_error("Queue capacity cannot be zero.");
        return NULL;
    }

    Queue* q = (Queue*)malloc(sizeof(Queue));
    if (!q) {
        // MODIFIED: Use the logging library
        log_fatal("Failed to allocate memory for queue structure: %s", strerror(errno));
        return NULL;
    }

    q->buffer = (void**)malloc(capacity * sizeof(void*));
    if (!q->buffer) {
        // MODIFIED: Use the logging library
        log_fatal("Failed to allocate memory for queue buffer: %s", strerror(errno));
        free(q);
        return NULL;
    }

    q->capacity = capacity;
    q->count = 0;
    q->head = 0;
    q->tail = 0;
    q->shutting_down = false;

    int ret;
    if ((ret = pthread_mutex_init(&q->mutex, NULL)) != 0) {
        // MODIFIED: Use the logging library
        log_fatal("pthread_mutex_init failed: %s", strerror(ret));
        free(q->buffer);
        free(q);
        return NULL;
    }

    if ((ret = pthread_cond_init(&q->not_empty_cond, NULL)) != 0) {
        // MODIFIED: Use the logging library
        log_fatal("pthread_cond_init (not_empty) failed: %s", strerror(ret));
        pthread_mutex_destroy(&q->mutex);
        free(q->buffer);
        free(q);
        return NULL;
    }

    if ((ret = pthread_cond_init(&q->not_full_cond, NULL)) != 0) {
        // MODIFIED: Use the logging library
        log_fatal("pthread_cond_init (not_full) failed: %s", strerror(ret));
        pthread_cond_destroy(&q->not_empty_cond);
        pthread_mutex_destroy(&q->mutex);
        free(q->buffer);
        free(q);
        return NULL;
    }

    return q;
}

/**
 * @brief Destroys a thread-safe queue.
 */
void queue_destroy(Queue* q) {
    if (!q) {
        return;
    }

    // It's the caller's responsibility to ensure no threads are using the queue.
    // This function does not free the items pointed to by the queue elements.
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty_cond);
    pthread_cond_destroy(&q->not_full_cond);

    free(q->buffer);
    free(q);
}

/**
 * @brief Enqueues an item onto the queue. Blocks if the queue is full.
 */
bool queue_enqueue(Queue* q, void* item) {
    if (!q) return false;

    pthread_mutex_lock(&q->mutex);

    // Wait while the queue is full AND not shutting down
    while (q->count == q->capacity && !q->shutting_down) {
        pthread_cond_wait(&q->not_full_cond, &q->mutex);
    }

    // If shutdown was signaled while waiting, do not enqueue and exit.
    if (q->shutting_down) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    q->buffer[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    // Signal that the queue is no longer empty
    pthread_cond_signal(&q->not_empty_cond);

    pthread_mutex_unlock(&q->mutex);
    return true;
}

/**
 * @brief Dequeues an item from the queue. Blocks if the queue is empty.
 */
void* queue_dequeue(Queue* q) {
    if (!q) return NULL;

    pthread_mutex_lock(&q->mutex);

    // Wait while the queue is empty AND not shutting down
    while (q->count == 0 && !q->shutting_down) {
        pthread_cond_wait(&q->not_empty_cond, &q->mutex);
    }

    // If shutdown was signaled while waiting, or if queue is empty AND shutting down,
    // return NULL to signal the end of the stream.
    if (q->shutting_down && q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    void* item = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    // Signal that the queue is no longer full
    pthread_cond_signal(&q->not_full_cond);

    pthread_mutex_unlock(&q->mutex);
    return item;
}

/**
 * @brief Signals all threads waiting on the queue to wake up and exit.
 */
void queue_signal_shutdown(Queue* q) {
     if (!q) return;

    pthread_mutex_lock(&q->mutex);
    q->shutting_down = true;
    // Broadcast to wake ALL waiting threads (on both conditions)
    pthread_cond_broadcast(&q->not_empty_cond);
    pthread_cond_broadcast(&q->not_full_cond);
    pthread_mutex_unlock(&q->mutex);
}
