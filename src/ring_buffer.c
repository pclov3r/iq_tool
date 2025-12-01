#include "ring_buffer.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h> // Added for Condition Variables
#include "log.h"

struct RingBuffer {
    unsigned char* buffer;
    size_t capacity;
    
    // C99 Lock-Free Implementation:
    // Volatile prevents register caching.
    volatile size_t write_pos;
    volatile size_t read_pos;
    
    volatile bool end_of_stream;
    volatile bool shutting_down;

    // Synchronization for Backpressure (Producer Waiting)
    // These are NOT used by the lock-free writer (SDR hardware thread).
    pthread_mutex_t sync_mutex;
    pthread_cond_t space_free_cond;
};

RingBuffer* ring_buffer_create(size_t capacity) {
    RingBuffer* iob = (RingBuffer*)malloc(sizeof(RingBuffer));
    if (!iob) {
        log_fatal("Failed to allocate memory for RingBuffer struct.");
        return NULL;
    }

    iob->buffer = (unsigned char*)malloc(capacity);
    if (!iob->buffer) {
        log_fatal("Failed to allocate memory for RingBuffer data buffer of size %zu bytes.", capacity);
        free(iob);
        return NULL;
    }

    iob->capacity = capacity;
    iob->write_pos = 0;
    iob->read_pos = 0;
    iob->end_of_stream = false;
    iob->shutting_down = false;

    // Initialize sync primitives for backpressure
    pthread_mutex_init(&iob->sync_mutex, NULL);
    pthread_cond_init(&iob->space_free_cond, NULL);

    log_info("I/O buffer created with %zu bytes capacity.", capacity);
    return iob;
}

void ring_buffer_destroy(RingBuffer* iob) {
    if (!iob) return;
    pthread_mutex_destroy(&iob->sync_mutex);
    pthread_cond_destroy(&iob->space_free_cond);
    free(iob->buffer);
    free(iob);
}

// PRODUCER: High Priority, Lock-Free, Never Sleeps
// Note: This function remains untouched by mutexes/cond-vars to ensure SDR safety.
size_t ring_buffer_write(RingBuffer* iob, const void* data, size_t bytes) {
    if (!iob || !data || bytes == 0) return 0;

    // 1. Load indices
    size_t w = iob->write_pos;
    size_t r = iob->read_pos;
    size_t cap = iob->capacity;

    // 2. Calculate available space (One slot reserved to distinguish full/empty)
    size_t size = (w >= r) ? (w - r) : (cap - (r - w));
    size_t available = (cap - 1) - size;

    // Drop data if full. We cannot block the hardware thread.
    if (bytes > available) {
        return 0; 
    }

    // 3. Copy Data
    size_t first_chunk_size = (w + bytes > cap) ? (cap - w) : bytes;
    memcpy(iob->buffer + w, data, first_chunk_size);

    size_t second_chunk_size = bytes - first_chunk_size;
    if (second_chunk_size > 0) {
        memcpy(iob->buffer, (const unsigned char*)data + first_chunk_size, second_chunk_size);
    }

    // 4. Barrier: Ensure data is committed before index update
    MEMORY_BARRIER();

    // 5. Update Write Index
    iob->write_pos = (w + bytes) % cap;

    return bytes;
}

// CONSUMER: Low Priority, Blocking (Sleeps if empty)
size_t ring_buffer_read(RingBuffer* iob, void* buffer, size_t max_bytes) {
    if (!iob || !buffer || max_bytes == 0) return 0;

    size_t w, r, cap, available;

    // --- BLOCKING LOOP ---
    // Wait until data arrives or stream ends
    while (true) {
        w = iob->write_pos;
        r = iob->read_pos;
        cap = iob->capacity;

        // Check shutdown first
        if (iob->shutting_down) return 0;

        // Calculate available
        available = (w >= r) ? (w - r) : (cap - (r - w));

        if (available > 0) {
            break; // Data found! Proceed to read.
        }

        // If empty AND end_of_stream is set, we are truly done.
        if (iob->end_of_stream) {
            return 0;
        }

        // Buffer empty, but stream alive. Sleep briefly to yield CPU.
        SLEEP_MS(1); 
    }

    // 1. Determine read size
    size_t bytes_to_read = (max_bytes > available) ? available : max_bytes;

    // 2. Barrier: Ensure we see the latest data written by producer
    MEMORY_BARRIER();

    // 3. Copy Data
    size_t first_chunk_size = (r + bytes_to_read > cap) ? (cap - r) : bytes_to_read;
    memcpy(buffer, iob->buffer + r, first_chunk_size);

    size_t second_chunk_size = bytes_to_read - first_chunk_size;
    if (second_chunk_size > 0) {
        memcpy((unsigned char*)buffer + first_chunk_size, iob->buffer, second_chunk_size);
    }

    // 4. Barrier: Ensure read is done before updating index
    MEMORY_BARRIER();

    // 5. Update Read Index
    iob->read_pos = (r + bytes_to_read) % cap;

    // 6. Signal Backpressure
    // We freed up space. Wake up any waiting producers (File Readers).
    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);

    return bytes_to_read;
}

// BACKPRESSURE WAIT: Used by File Readers (Producers)
void ring_buffer_wait_for_threshold(RingBuffer* iob, size_t target_size) {
    if (!iob) return;

    pthread_mutex_lock(&iob->sync_mutex);
    // While buffer is too full, wait for Consumer to signal space freed.
    while (!iob->shutting_down && ring_buffer_get_size(iob) > target_size) {
        pthread_cond_wait(&iob->space_free_cond, &iob->sync_mutex);
    }
    pthread_mutex_unlock(&iob->sync_mutex);
}

void ring_buffer_signal_end_of_stream(RingBuffer* iob) {
    if (!iob) return;
    iob->end_of_stream = true;
    // Wake up any backpressure waiters so they can exit/finish
    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);
}

void ring_buffer_signal_shutdown(RingBuffer* iob) {
    if (!iob) return;
    iob->shutting_down = true;
    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);
}

size_t ring_buffer_get_size(RingBuffer* iob) {
    if (!iob) return 0;
    size_t w = iob->write_pos;
    size_t r = iob->read_pos;
    if (w >= r) return w - r;
    return iob->capacity - (r - w);
}

size_t ring_buffer_get_capacity(RingBuffer* iob) {
    if (!iob) return 0;
    return iob->capacity;
}
