#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "log.h"

#ifdef _WIN32
    #include <windows.h>
    #define MEMORY_BARRIER() MemoryBarrier()
    #define SLEEP_MS(x) Sleep(x)
#elif defined(__GNUC__) || defined(__clang__)
    #include <unistd.h>
    #define MEMORY_BARRIER() __sync_synchronize()
    #define SLEEP_MS(x) usleep((x) * 1000)
#else
    #error "Compiler not supported for lock-free ring buffer."
#endif

struct RingBuffer {
    unsigned char* buffer;
    size_t capacity;
    
    // C99 Lock-Free Implementation:
    // Volatile prevents register caching.
    volatile size_t write_pos;
    volatile size_t read_pos;
    
    volatile bool end_of_stream;
    volatile bool shutting_down;
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

    log_debug("I/O buffer created with %zu bytes capacity (Lock-Free Writer / Blocking Reader).", capacity);
    return iob;
}

void ring_buffer_destroy(RingBuffer* iob) {
    if (!iob) return;
    free(iob->buffer);
    free(iob);
}

// PRODUCER: High Priority, Lock-Free, Never Sleeps
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
        // This replaces the Condition Variable wait.
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

    return bytes_to_read;
}

void ring_buffer_signal_end_of_stream(RingBuffer* iob) {
    if (!iob) return;
    iob->end_of_stream = true;
}

void ring_buffer_signal_shutdown(RingBuffer* iob) {
    if (!iob) return;
    iob->shutting_down = true;
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
