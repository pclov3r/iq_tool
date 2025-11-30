#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_

#include <stddef.h>
#include <stdbool.h>

// --- Opaque Structure Definition ---
typedef struct RingBuffer RingBuffer;

// --- Function Declarations ---

/**
 * @brief Creates a new I/O ring buffer.
 * @param capacity The total size of the buffer in bytes.
 * @return A pointer to the new RingBuffer, or NULL on failure.
 */
RingBuffer* ring_buffer_create(size_t capacity);

/**
 * @brief Destroys an I/O buffer and frees all associated memory.
 */
void ring_buffer_destroy(RingBuffer* iob);

/**
 * @brief Writes data to the I/O buffer. (Producer-side Function)
 *
 * This is a LOCK-FREE, NON-BLOCKING call. 
 * It will NEVER sleep or lock a mutex. Ideally suited for hardware callbacks.
 * If the buffer is full, it returns 0 (drops data) to maintain real-time timing.
 *
 * @return The number of bytes successfully written.
 */
size_t ring_buffer_write(RingBuffer* iob, const void* data, size_t bytes);

/**
 * @brief Reads data from the I/O buffer. (Consumer-side Function)
 *
 * This is a BLOCKING call (Sleep-Wait). 
 * It waits efficiently for data to become available.
 * It returns 0 ONLY when the stream has ended or shutdown is requested.
 *
 * @return The number of bytes actually read.
 */
size_t ring_buffer_read(RingBuffer* iob, void* buffer, size_t max_bytes);

/**
 * @brief Signals that no more data will be written to the buffer.
 */
void ring_buffer_signal_end_of_stream(RingBuffer* iob);

/**
 * @brief Signals an immediate shutdown of the buffer.
 */
void ring_buffer_signal_shutdown(RingBuffer* iob);

/**
 * @brief Gets the current number of bytes waiting to be read.
 */
size_t ring_buffer_get_size(RingBuffer* iob);

/**
 * @brief Gets the total capacity of the buffer.
 */
size_t ring_buffer_get_capacity(RingBuffer* iob);

#endif // RING_BUFFER_H_
