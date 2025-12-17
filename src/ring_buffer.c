#include "ring_buffer.h"
#include "platform.h"
#include "constants.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include "log.h"

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#endif

struct RingBuffer {
    unsigned char* buffer;
    size_t capacity;

    // --- PADDING 1 ---
    // Isolates the read-only configuration from the write position.
    char _pad1[64];

    // C99 Lock-Free Implementation:
    // Volatile prevents register caching.
    volatile size_t write_pos;

    // --- PADDING 2 ---
    // Pushes 'read_pos' into a completely different cache line
    // than 'write_pos'. Prevents False Sharing between Producer and Consumer cores.
    char _pad2[64];

    volatile size_t read_pos;

    // --- PADDING 3 ---
    // Isolates the reader index from the flags and mutexes below.
    char _pad3[64];

    volatile bool end_of_stream;
    volatile bool shutting_down;

    // Synchronization for Backpressure (Producer Waiting)
    // These are NOT used by the lock-free writer (SDR hardware thread).
    pthread_mutex_t sync_mutex;
    pthread_cond_t space_free_cond;

    // Synchronization for Latency (Consumer Waiting)
    // Used to wake up the reader immediately when a block arrives.
#ifdef _WIN32
    HANDLE block_ready_event;
#else
    sem_t block_ready_sem;
#endif
};

RingBuffer* ring_buffer_create(size_t capacity) {
    RingBuffer* iob = (RingBuffer*)malloc(sizeof(RingBuffer));
    if (!iob) {
        log_fatal("Failed to allocate memory for RingBuffer struct.");
        return NULL;
    }

#ifdef _WIN32
    iob->buffer = (unsigned char*)_aligned_malloc(capacity, MEM_ARENA_ALIGNMENT);
#else
    void* ptr = NULL;
    if (posix_memalign(&ptr, MEM_ARENA_ALIGNMENT, capacity) != 0) {
        ptr = NULL;
    }
    iob->buffer = (unsigned char*)ptr;
#endif

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

    pthread_mutex_init(&iob->sync_mutex, NULL);
    pthread_cond_init(&iob->space_free_cond, NULL);

#ifdef _WIN32
    // reset event, initially nonsignaled
    iob->block_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!iob->block_ready_event) {
        log_fatal("Failed to create RingBuffer event.");
        return NULL;
    }
#else
    // semaphore shared between threads (0), initial value 0
    if (sem_init(&iob->block_ready_sem, 0, 0) != 0) {
        log_fatal("Failed to initialize RingBuffer semaphore.");
        return NULL;
    }
#endif

    log_info("I/O buffer created with %zu bytes capacity.", capacity);
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
    sem_destroy(&iob->block_ready_sem);
#endif

    if (iob->buffer) {
#ifdef _WIN32
        _aligned_free(iob->buffer);
#else
        free(iob->buffer);
#endif
    }
    free(iob);
}

// PRODUCER: High Priority, Lock-Free, Never Sleeps
size_t ring_buffer_write(RingBuffer* iob, const void* data, size_t bytes) {
    if (!iob || !data || bytes == 0) return 0;

    size_t w = iob->write_pos;
    size_t r = iob->read_pos;
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

    MEMORY_BARRIER();

    iob->write_pos = (w + bytes) % cap;

    // Wake up the reader immediately (Non-blocking signal)
#ifdef _WIN32
    SetEvent(iob->block_ready_event);
#else
    sem_post(&iob->block_ready_sem);
#endif

    return bytes;
}

// CONSUMER: Low Priority, Blocking (Sleeps if empty)
size_t ring_buffer_read(RingBuffer* iob, void* buffer, size_t max_bytes) {
    if (!iob || !buffer || max_bytes == 0) return 0;

    size_t w, r, cap, available;

    while (true) {
        w = iob->write_pos;
        r = iob->read_pos;
        cap = iob->capacity;

        if (iob->shutting_down) return 0;

        available = (w >= r) ? (w - r) : (cap - (r - w));

        if (available > 0) {
            break;
        }

        if (iob->end_of_stream) {
            return 0;
        }

        // Buffer empty, wait efficiently for the producer signal
#ifdef _WIN32
        WaitForSingleObject(iob->block_ready_event, 100);
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; // 100ms timeout
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        sem_timedwait(&iob->block_ready_sem, &ts);
#endif
    }

    size_t bytes_to_read = (max_bytes > available) ? available : max_bytes;

    MEMORY_BARRIER();

    size_t first_chunk_size = (r + bytes_to_read > cap) ? (cap - r) : bytes_to_read;
    memcpy(buffer, iob->buffer + r, first_chunk_size);

    size_t second_chunk_size = bytes_to_read - first_chunk_size;
    if (second_chunk_size > 0) {
        memcpy((unsigned char*)buffer + first_chunk_size, iob->buffer, second_chunk_size);
    }

    MEMORY_BARRIER();

    iob->read_pos = (r + bytes_to_read) % cap;

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
    while (!iob->shutting_down && ring_buffer_get_size(iob) > target_size) {
        pthread_cond_wait(&iob->space_free_cond, &iob->sync_mutex);
    }
    pthread_mutex_unlock(&iob->sync_mutex);
}

void ring_buffer_signal_end_of_stream(RingBuffer* iob) {
    if (!iob) return;
    iob->end_of_stream = true;

    // Wake up backpressure waiters
    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);

    // Wake up consumer
#ifdef _WIN32
    SetEvent(iob->block_ready_event);
#else
    sem_post(&iob->block_ready_sem);
#endif
}

void ring_buffer_signal_shutdown(RingBuffer* iob) {
    if (!iob) return;
    iob->shutting_down = true;

    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);

    // Wake up consumer
#ifdef _WIN32
    SetEvent(iob->block_ready_event);
#else
    sem_post(&iob->block_ready_sem);
#endif
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
