#include "queue.h"
#include "log.h"
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
        log_error("Queue capacity cannot be zero.");
        return NULL;
    }

    Queue* queue = (Queue*)malloc(sizeof(Queue));
    if (!queue) {
        log_fatal("Failed to allocate memory for queue structure: %s", strerror(errno));
        return NULL;
    }

    queue->buffer = (void**)malloc(capacity * sizeof(void*));
    if (!queue->buffer) {
        log_fatal("Failed to allocate memory for queue buffer: %s", strerror(errno));
        free(queue);
        return NULL;
    }

    queue->capacity = capacity;
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->shutting_down = false;

    int ret;
    if ((ret = pthread_mutex_init(&queue->mutex, NULL)) != 0) {
        log_fatal("pthread_mutex_init failed: %s", strerror(ret));
        free(queue->buffer);
        free(queue);
        return NULL;
    }
    if ((ret = pthread_cond_init(&queue->not_empty_cond, NULL)) != 0) {
        log_fatal("pthread_cond_init (not_empty) failed: %s", strerror(ret));
        pthread_mutex_destroy(&queue->mutex);
        free(queue->buffer);
        free(queue);
        return NULL;
    }
    if ((ret = pthread_cond_init(&queue->not_full_cond, NULL)) != 0) {
        log_fatal("pthread_cond_init (not_full) failed: %s", strerror(ret));
        pthread_cond_destroy(&queue->not_empty_cond);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->buffer);
        free(queue);
        return NULL;
    }

    return queue;
}

/**
 * @brief Destroys a thread-safe queue.
 */
void queue_destroy(Queue* queue) {
    if (!queue) {
        return;
    }
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty_cond);
    pthread_cond_destroy(&queue->not_full_cond);
    free(queue->buffer);
    free(queue);
}

/**
 * @brief Enqueues an item onto the queue. Blocks if the queue is full.
 */
bool queue_enqueue(Queue* queue, void* item) {
    if (!queue) return false;

    pthread_mutex_lock(&queue->mutex);

    while (queue->count == queue->capacity && !queue->shutting_down) {
        pthread_cond_wait(&queue->not_full_cond, &queue->mutex);
    }

    if (queue->shutting_down) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    queue->buffer[queue->tail] = item;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    pthread_cond_signal(&queue->not_empty_cond);
    pthread_mutex_unlock(&queue->mutex);

    return true;
}

/**
 * @brief Dequeues an item from the queue. Blocks if the queue is empty.
 */
void* queue_dequeue(Queue* queue) {
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->mutex);

    while (queue->count == 0 && !queue->shutting_down) {
        pthread_cond_wait(&queue->not_empty_cond, &queue->mutex);
    }

    if (queue->shutting_down && queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    void* item = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    pthread_cond_signal(&queue->not_full_cond);
    pthread_mutex_unlock(&queue->mutex);

    return item;
}

/**
 * @brief Attempts to dequeue an item from the queue without blocking.
 */
void* queue_try_dequeue(Queue* queue) {
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->mutex);

    if (queue->count == 0 || queue->shutting_down) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    void* item = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    pthread_cond_signal(&queue->not_full_cond);
    pthread_mutex_unlock(&queue->mutex);

    return item;
}


/**
 * @brief Signals all threads waiting on the queue to wake up and exit.
 */
void queue_signal_shutdown(Queue* queue) {
    if (!queue) return;

    pthread_mutex_lock(&queue->mutex);
    queue->shutting_down = true;

    pthread_cond_broadcast(&queue->not_empty_cond);
    pthread_cond_broadcast(&queue->not_full_cond);

    pthread_mutex_unlock(&queue->mutex);
}
