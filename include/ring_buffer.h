#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_

#include <stddef.h>
#include <stdbool.h>

// --- Opaque Structure Definition ---
// By defining the struct in the .c file, we hide its implementation details
// from the rest of the application, which is a good encapsulation practice.
typedef struct RingBuffer RingBuffer;


// --- Function Declarations ---

/**
 * @brief Creates a new I/O ring buffer.
 *
 * This function allocates a large, contiguous block of memory to serve as a
 * thread-safe, circular buffer for decoupling the real-time pipeline from
 * the disk writer.
 *
 * @param capacity The total size of the buffer in bytes. A large size (e.g., 1GB)
 *                 is recommended to absorb significant I/O latency spikes.
 * @return A pointer to the new RingBuffer, or NULL on memory allocation failure.
 */
RingBuffer* ring_buffer_create(size_t capacity);

/**
 * @brief Destroys an I/O buffer and frees all associated memory.
 * @param iob The I/O buffer to destroy.
 */
void ring_buffer_destroy(RingBuffer* iob);

/**
 * @brief Writes data to the I/O buffer. (Producer-side Function)
 *
 * This is a NON-BLOCKING call. It is designed to be called from a real-time
 * thread. If the buffer does not have enough free space to write all the
 * requested bytes, it will write as much as it can and return immediately.
 *
 * @param iob The I/O buffer.
 * @param data A pointer to the data to be written.
 * @param bytes The number of bytes to write.
 * @return The number of bytes actually written to the buffer. If this value is
 *         less than 'bytes', it indicates a buffer overrun has occurred.
 */
size_t ring_buffer_write(RingBuffer* iob, const void* data, size_t bytes);

/**
 * @brief Reads data from the I/O buffer. (Consumer-side Function)
 *
 * This is a BLOCKING call. It is designed to be called from the writer thread.
 * It will wait efficiently (by sleeping) until data becomes available or until
 * the end-of-stream or a shutdown is signaled.
 *
 * @param iob The I/O buffer.
 * @param buffer A pointer to a local buffer to read the data into.
 * @param max_bytes The maximum number of bytes to read (typically the size of 'buffer').
 * @return The number of bytes actually read. Returns 0 if the end of the stream
 *         is reached and the buffer is empty, or if a shutdown is signaled.
 */
size_t ring_buffer_read(RingBuffer* iob, void* buffer, size_t max_bytes);

/**
 * @brief Signals that no more data will be written to the buffer.
 *
 * This should be called by the producer thread after it has written its last
 * piece of data. This allows the consumer thread to exit its loop cleanly
 * after it has finished reading all remaining data from the buffer.
 *
 * @param iob The I/O buffer.
 */
void ring_buffer_signal_end_of_stream(RingBuffer* iob);

/**
 * @brief Signals an immediate shutdown of the buffer.
 *
 * This function will unblock any waiting consumer thread, causing it to
 * return 0 immediately, even if there is data in the buffer. This is used
 * for a fast exit on events like Ctrl+C.
 *
 * @param iob The I/O buffer.
 */
void ring_buffer_signal_shutdown(RingBuffer* iob);

/**
 * @brief Gets the current number of bytes waiting to be read in the buffer.
 * @param iob The I/O buffer.
 * @return The number of bytes currently in the buffer.
 */
size_t ring_buffer_get_size(RingBuffer* iob);

/**
 * @brief Gets the total capacity of the buffer.
 * @param iob The I/O buffer.
 * @return The total capacity in bytes.
 */
size_t ring_buffer_get_capacity(RingBuffer* iob);

#endif // RING_BUFFER_H_
