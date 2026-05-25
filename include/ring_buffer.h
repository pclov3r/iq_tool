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
struct MemoryArena;

RingBuffer* ring_buffer_create(size_t capacity, struct MemoryArena* arena);

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
 * @brief Scatter-Gather write for lock-free atomicity.
 *
 * Writes a header and a payload in a single atomic operation.
 * Prevents the consumer from waking up before the entire packet is written.
 *
 * @return The total number of bytes successfully written, or 0 if dropped.
 */
size_t ring_buffer_write_packet(RingBuffer* iob, const void* header, size_t h_len, const void* payload, size_t p_len);

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
 * @brief Blocks the calling thread until the buffer usage drops below a target.
 *
 * This is used for BACKPRESSURE (e.g. File Readers). It sleeps efficiently
 * on a condition variable until the Consumer clears enough space.
 *
 * @param iob The I/O buffer.
 * @param target_size The size (in bytes) to wait for. Returns when current_size <= target_size.
 */
void ring_buffer_wait_for_threshold(RingBuffer* iob, size_t target_size);

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
