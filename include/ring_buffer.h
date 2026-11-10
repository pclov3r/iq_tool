/**
 * @file ring_buffer.h
 */

#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_

#include <stddef.h>

/**
 * @enum RingBufferState
 * @brief Lifecycle and thread-parking states for the lock-free ring buffer.
 */
typedef enum RingBufferState {
  RING_BUFFER_STATE_UNINITIALIZED = 0,
  RING_BUFFER_STATE_ACTIVE,           ///< Streaming: both threads active.
  RING_BUFFER_STATE_CONSUMER_WAITING, ///< Empty: consumer parked waiting for data.
  RING_BUFFER_STATE_PRODUCER_WAITING, ///< Full: producer parked waiting for space.
  RING_BUFFER_STATE_DRAINING,         ///< End of stream: reject writes, reader drains.
  RING_BUFFER_STATE_SHUTDOWN,         ///< Immediate shutdown: abort reader and writer.
  RING_BUFFER_STATE_DESTROYED         ///< Destroyed: completely inert.
} RingBufferState;

// --- Opaque Structure Definition ---
typedef struct RingBuffer RingBuffer;

// --- Function Declarations ---

/**
 * @brief Creates a new I/O ring buffer.
 * @param capacity The total size of the buffer in bytes.
 * @return A pointer to the new RingBuffer, or NULL on failure.
 */
struct MemoryArena;

RingBuffer *ring_buffer_create(size_t capacity, struct MemoryArena *arena);

/**
 * @brief Destroys an I/O buffer and frees all associated memory.
 */
void ring_buffer_destroy(RingBuffer *ring_buffer);

/**
 * @brief Writes data to the I/O buffer. (Producer-side Function)
 *
 * This is a LOCK-FREE, NON-BLOCKING call.
 * It will NEVER sleep or lock a mutex. Ideally suited for hardware callbacks.
 * If the buffer is full, it returns 0 (drops data) to maintain real-time
 * timing.
 *
 * @return The number of bytes successfully written.
 */
size_t ring_buffer_write(RingBuffer *ring_buffer, const void *data, size_t bytes);

/**
 * @brief Scatter-Gather write for lock-free atomicity.
 *
 * Writes a header and a payload in a single atomic operation.
 * Prevents the consumer from waking up before the entire packet is written.
 *
 * @return The total number of bytes successfully written, or 0 if dropped.
 */
size_t ring_buffer_write_packet(RingBuffer *ring_buffer, const void *header,
                                size_t header_length, const void *payload,
                                size_t payload_length);

/**
 * @brief Reads data from the I/O buffer. (Consumer-side Function)
 *
 * This is a BLOCKING call (Sleep-Wait).
 * It waits efficiently for data to become available.
 * It returns 0 ONLY when the stream has ended or shutdown is requested.
 *
 * @return The number of bytes actually read.
 */
size_t ring_buffer_read(RingBuffer *ring_buffer, void *buffer, size_t max_bytes);

/**
 * @brief Reads available data from the I/O buffer without blocking.
 *
 * This is a LOCK-FREE, NON-BLOCKING call safe for real-time audio callbacks.
 * Returns immediately with whatever data is currently available up to max_bytes,
 * or 0 if empty or shutting down.
 *
 * @param ring_buffer The I/O buffer.
 * @param buffer Output destination buffer.
 * @param max_bytes Maximum number of bytes to read.
 * @return The number of bytes actually read.
 */
size_t ring_buffer_try_read(RingBuffer *ring_buffer, void *buffer,
                            size_t max_bytes);

/**
 * @brief Blocks the calling thread until the buffer usage drops below a target.
 *
 * This is used for BACKPRESSURE (e.g. File Readers). It sleeps efficiently
 * on a wake signal until the Consumer clears enough space.
 *
 * @param ring_buffer The I/O buffer.
 * @param target_size The size (in bytes) to wait for. Returns when current_size
 * <= target_size.
 */
void ring_buffer_wait_for_threshold(RingBuffer *ring_buffer, size_t target_size);

/**
 * @brief Transitions buffer to DRAINING state (no more writes; consumer drains
 * remaining data).
 */
void ring_buffer_drain(RingBuffer *ring_buffer);

/**
 * @brief Transitions buffer to SHUTDOWN state (immediate abort; wakes all
 * waiters).
 */
void ring_buffer_shutdown(RingBuffer *ring_buffer);

/**
 * @brief Gets the current number of bytes waiting to be read.
 */
size_t ring_buffer_get_size(RingBuffer *ring_buffer);

/**
 * @brief Gets the total capacity of the buffer.
 */
size_t ring_buffer_get_capacity(RingBuffer *ring_buffer);

/**
 * @brief Clears the ring buffer.
 */
void ring_buffer_clear(RingBuffer *ring_buffer);

/**
 * @brief Returns the current lifecycle state of the ring buffer.
 */
RingBufferState ring_buffer_get_state(const RingBuffer *ring_buffer);

#endif // RING_BUFFER_H_
