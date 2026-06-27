/**
 * @file queue.c
 */

#include "queue.h"
#include "log.h"
#include "mem_arena.h" // For mem_arena_alloc
#include <string.h>

bool queue_init(Queue* queue, size_t capacity, MemoryArena* arena) {
    if (!queue || !arena) {
        log_error("queue_init received NULL pointer.");
        return false;
    }
    if (capacity == 0) {
        log_error("Queue capacity cannot be zero.");
        return false;
    }

    // Allocate the internal buffer from the memory arena
    queue->buffer = (void**)mem_arena_alloc(arena, capacity * sizeof(void*), false);
    if (!queue->buffer) {
        // mem_arena_alloc will have already logged the fatal error
        return false;
    }

    queue->capacity = capacity;
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->shutting_down = false;

    int return_code;
    if ((return_code = pthread_mutex_init(&queue->mutex, NULL)) != 0) {
        log_fatal("pthread_mutex_init failed: %s", strerror(return_code));
        return false;
    }
    if ((return_code = pthread_cond_init(&queue->not_empty_cond, NULL)) != 0) {
        log_fatal("pthread_cond_init (not_empty) failed: %s", strerror(return_code));
        pthread_mutex_destroy(&queue->mutex);
        return false;
    }
    if ((return_code = pthread_cond_init(&queue->not_full_cond, NULL)) != 0) {
        log_fatal("pthread_cond_init (not_full) failed: %s", strerror(return_code));
        pthread_cond_destroy(&queue->not_empty_cond);
        pthread_mutex_destroy(&queue->mutex);
        return false;
    }

    queue->is_initialized = true;
    return true;
}

void queue_destroy(Queue* queue) {
    if (!queue || !queue->is_initialized) {
        return;
    }
    queue->is_initialized = false;
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty_cond);
    pthread_cond_destroy(&queue->not_full_cond);
}

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

    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->not_empty_cond);

    return true;
}

bool queue_enqueue_forced(Queue* queue, void* item) {
    if (!queue) return false;
    pthread_mutex_lock(&queue->mutex);

    // Wait for space (sanity check), but IGNORE shutting_down flag
    if (queue->count == queue->capacity) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    queue->buffer[queue->tail] = item;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->not_empty_cond);
    return true;
}

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

    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->not_full_cond);

    return item;
}

void* queue_try_dequeue(Queue* queue) {
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->mutex);

    if (queue->count == 0)  {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    void* item = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->not_full_cond);

    return item;
}

void queue_signal_shutdown(Queue* queue) {
    if (!queue) return;

    pthread_mutex_lock(&queue->mutex);
    queue->shutting_down = true;

    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_broadcast(&queue->not_empty_cond);
    pthread_cond_broadcast(&queue->not_full_cond);
}
