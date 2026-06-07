/**
 * @file ring_buffer.c
 */

#include "ring_buffer.h"
#include "mem_arena.h"
#include "platform.h"
#include "constants.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h> // C11 Atomics
#include <stdalign.h>  // C11 Alignment
#include "log.h"

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <time.h>
#include <errno.h>
#endif

struct RingBuffer {
    unsigned char* buffer;
    size_t capacity;

    // C11 Lock-Free Implementation:
    // alignas(128) pushes 'write_pos' and 'read_pos' into completely different
    // cache lines (and prevents spatial prefetcher collisions).
    // This permanently eliminates False Sharing between Producer and Consumer cores.
    alignas(128) atomic_size_t write_pos;
    alignas(128) atomic_size_t read_pos;

    // Isolates the reader index from the flags and mutexes below.
    alignas(128) atomic_bool end_of_stream;
    atomic_bool shutting_down;

    // Synchronization for Backpressure (Producer Waiting)
    // These are NOT used by the lock-free writer (SDR hardware thread).
    pthread_mutex_t sync_mutex;
    pthread_cond_t space_free_cond;

    // Synchronization for Latency (Consumer Waiting)
    // Used to wake up the reader immediately when a block arrives.
#ifdef _WIN32
    HANDLE block_ready_event;
#else
    // Replaced sem_t with manual Auto-Reset Event logic to match Windows behavior
    // and ensure CLOCK_MONOTONIC usage (immune to time jumps).
    pthread_mutex_t event_mutex;
    pthread_cond_t  event_cond;
    bool            event_signaled;
#endif
};

RingBuffer* ring_buffer_create(size_t capacity, MemoryArena* arena) {
    RingBuffer* iob = (RingBuffer*)mem_arena_alloc(arena, sizeof(RingBuffer), true);
    if (!iob) {
        log_fatal("Failed to allocate memory for RingBuffer struct from arena.");
        return NULL;
    }

    size_t aligned_capacity = (capacity + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);

    iob->buffer = (unsigned char*)mem_arena_alloc(arena, aligned_capacity, false);
    if (!iob->buffer) {
        log_fatal("Failed to allocate memory for RingBuffer data buffer from arena.");
        return NULL;
    }

    iob->capacity = aligned_capacity;
    atomic_init(&iob->write_pos, 0);
    atomic_init(&iob->read_pos, 0);
    atomic_init(&iob->end_of_stream, false);
    atomic_init(&iob->shutting_down, false);

    pthread_mutex_init(&iob->sync_mutex, NULL);
    pthread_cond_init(&iob->space_free_cond, NULL);

#ifdef _WIN32
    iob->block_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!iob->block_ready_event) {
        log_fatal("Failed to create RingBuffer event.");
        return NULL;
    }
#else
    if (pthread_mutex_init(&iob->event_mutex, NULL) != 0) {
        log_fatal("Failed to initialize RingBuffer event mutex.");
        return NULL;
    }

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);

    if (pthread_cond_init(&iob->event_cond, &attr) != 0) {
        log_fatal("Failed to initialize RingBuffer event cond.");
        pthread_condattr_destroy(&attr);
        return NULL;
    }
    pthread_condattr_destroy(&attr);

    iob->event_signaled = false;
#endif

    return iob;
}

void ring_buffer_destroy(RingBuffer* iob) {
    if (!iob) return;

    pthread_mutex_destroy(&iob->sync_mutex);
    pthread_cond_destroy(&iob->space_free_cond);

#ifdef _WIN32
    if (iob->block_ready_event) {
        CloseHandle(iob->block_ready_event);
    }
#else
    pthread_mutex_destroy(&iob->event_mutex);
    pthread_cond_destroy(&iob->event_cond);
#endif
}

// PRODUCER: High Priority, Lock-Free, Never Sleeps
size_t ring_buffer_write(RingBuffer* iob, const void* data, size_t bytes) {
    if (!iob || !data || bytes == 0) return 0;

    size_t w = atomic_load_explicit(&iob->write_pos, memory_order_relaxed);
    size_t r = atomic_load_explicit(&iob->read_pos, memory_order_acquire);
    size_t cap = iob->capacity;

    size_t size = (w >= r) ? (w - r) : (cap - (r - w));
    size_t available = (cap - 1) - size;

    if (bytes > available) {
        return 0;
    }

    size_t first_chunk_size = (w + bytes > cap) ? (cap - w) : bytes;
    memcpy(iob->buffer + w, data, first_chunk_size);

    size_t second_chunk_size = bytes - first_chunk_size;
    if (second_chunk_size > 0) {
        memcpy(iob->buffer, (const unsigned char*)data + first_chunk_size, second_chunk_size);
    }

    atomic_store_explicit(&iob->write_pos, (w + bytes) % cap, memory_order_release);

    // Wake up the reader immediately (Non-blocking signal)
#ifdef _WIN32
    SetEvent(iob->block_ready_event);
#else
    pthread_mutex_lock(&iob->event_mutex);
    iob->event_signaled = true; // Latch the signal
    pthread_cond_signal(&iob->event_cond);
    pthread_mutex_unlock(&iob->event_mutex);
#endif

    return bytes;
}

// PRODUCER: High Priority, Lock-Free, Scatter-Gather
size_t ring_buffer_write_packet(RingBuffer* iob, const void* header, size_t h_length, const void* payload, size_t p_length) {
    if (!iob || !header) return 0;

    size_t total_bytes = h_length + p_length;
    size_t w = atomic_load_explicit(&iob->write_pos, memory_order_relaxed);
    size_t r = atomic_load_explicit(&iob->read_pos, memory_order_acquire);
    size_t cap = iob->capacity;

    size_t size = (w >= r) ? (w - r) : (cap - (r - w));
    size_t available = (cap - 1) - size;

    if (total_bytes > available) {
        return 0; // Drop packet
    }

    // 1. Write Header
    size_t h_chunk1 = (w + h_length > cap) ? (cap - w) : h_length;
    memcpy(iob->buffer + w, header, h_chunk1);

    size_t h_chunk2 = h_length - h_chunk1;
    if (h_chunk2 > 0) {
        memcpy(iob->buffer, (const unsigned char*)header + h_chunk1, h_chunk2);
    }
    w = (w + h_length) % cap;

    // 2. Write Payload (if exists)
    if (payload && p_length > 0) {
        size_t p_chunk1 = (w + p_length > cap) ? (cap - w) : p_length;
        memcpy(iob->buffer + w, payload, p_chunk1);

        size_t p_chunk2 = p_length - p_chunk1;
        if (p_chunk2 > 0) {
            memcpy(iob->buffer, (const unsigned char*)payload + p_chunk1, p_chunk2);
        }
        w = (w + p_length) % cap;
    }

    // 3. Update write_pos and signal ONCE
    atomic_store_explicit(&iob->write_pos, w, memory_order_release);

    // Wake up the reader immediately (Non-blocking signal)
#ifdef _WIN32
    SetEvent(iob->block_ready_event);
#else
    pthread_mutex_lock(&iob->event_mutex);
    iob->event_signaled = true; // Latch the signal
    pthread_cond_signal(&iob->event_cond);
    pthread_mutex_unlock(&iob->event_mutex);
#endif

    return total_bytes;
}

// CONSUMER: Low Priority, Blocking (Sleeps if empty)
size_t ring_buffer_read(RingBuffer* iob, void* buffer, size_t max_bytes) {
    if (!iob || !buffer || max_bytes == 0) return 0;

    size_t w, r, cap, available;

    while (true) {
        w = atomic_load_explicit(&iob->write_pos, memory_order_acquire);
        r = atomic_load_explicit(&iob->read_pos, memory_order_relaxed);
        cap = iob->capacity;

        if (atomic_load_explicit(&iob->shutting_down, memory_order_relaxed)) return 0;

        available = (w >= r) ? (w - r) : (cap - (r - w));

        if (available > 0) {
            break;
        }

        if (atomic_load_explicit(&iob->end_of_stream, memory_order_acquire)) {
            return 0;
        }

        // Buffer empty, wait efficiently for the producer signal
#ifdef _WIN32
        WaitForSingleObject(iob->block_ready_event, 100);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts); // Safe Clock

        // Add 100ms
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }

        pthread_mutex_lock(&iob->event_mutex);

        // Auto-Reset Event Logic:
        // Wait until signaled or timeout.
        while (!iob->event_signaled) {
            int rc = pthread_cond_timedwait(&iob->event_cond, &iob->event_mutex, &ts);
            if (rc == ETIMEDOUT) break;
        }

        // Clear flag immediately (Auto-Reset) so we sleep again next time
        // unless producer signals again. This prevents the "Counting Semaphore Spin".
        iob->event_signaled = false;

        pthread_mutex_unlock(&iob->event_mutex);
#endif
    }

    size_t bytes_to_read = (max_bytes > available) ? available : max_bytes;

    size_t first_chunk_size = (r + bytes_to_read > cap) ? (cap - r) : bytes_to_read;
    memcpy(buffer, iob->buffer + r, first_chunk_size);

    size_t second_chunk_size = bytes_to_read - first_chunk_size;
    if (second_chunk_size > 0) {
        memcpy((unsigned char*)buffer + first_chunk_size, iob->buffer, second_chunk_size);
    }

    atomic_store_explicit(&iob->read_pos, (r + bytes_to_read) % cap, memory_order_release);

    // Signal Backpressure: Wake up any waiting producers (File Readers)
    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);

    return bytes_to_read;
}

// BACKPRESSURE WAIT: Used by File Readers (Producers)
void ring_buffer_wait_for_threshold(RingBuffer* iob, size_t target_size) {
    if (!iob) return;

    pthread_mutex_lock(&iob->sync_mutex);
    while (!atomic_load_explicit(&iob->shutting_down, memory_order_relaxed) && ring_buffer_get_size(iob) > target_size) {
        pthread_cond_wait(&iob->space_free_cond, &iob->sync_mutex);
    }
    pthread_mutex_unlock(&iob->sync_mutex);
}

void ring_buffer_signal_end_of_stream(RingBuffer* iob) {
    if (!iob) return;
    atomic_store_explicit(&iob->end_of_stream, true, memory_order_release);

    // Wake up backpressure waiters
    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);

    // Wake up consumer
#ifdef _WIN32
    SetEvent(iob->block_ready_event);
#else
    pthread_mutex_lock(&iob->event_mutex);
    iob->event_signaled = true;
    pthread_cond_signal(&iob->event_cond);
    pthread_mutex_unlock(&iob->event_mutex);
#endif
}

void ring_buffer_signal_shutdown(RingBuffer* iob) {
    if (!iob) return;
    atomic_store_explicit(&iob->shutting_down, true, memory_order_release);

    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);

    // Wake up consumer
#ifdef _WIN32
    SetEvent(iob->block_ready_event);
#else
    pthread_mutex_lock(&iob->event_mutex);
    iob->event_signaled = true;
    pthread_cond_signal(&iob->event_cond);
    pthread_mutex_unlock(&iob->event_mutex);
#endif
}

size_t ring_buffer_get_size(RingBuffer* iob) {
    if (!iob) return 0;
    size_t w = atomic_load_explicit(&iob->write_pos, memory_order_acquire);
    size_t r = atomic_load_explicit(&iob->read_pos, memory_order_acquire);
    if (w >= r) return w - r;
    return iob->capacity - (r - w);
}

size_t ring_buffer_get_capacity(RingBuffer* iob) {
    if (!iob) return 0;
    return iob->capacity;
}

void ring_buffer_clear(RingBuffer* iob) {
    if (!iob) return;
    atomic_store_explicit(&iob->read_pos, 0, memory_order_release);
    atomic_store_explicit(&iob->write_pos, 0, memory_order_release);

    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);
}
