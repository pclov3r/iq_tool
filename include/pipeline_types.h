/**
 * @file pipeline_types.h
 * @brief Defines data structures used for communication within the processing pipeline.
 *
 * This header contains the definitions for the core components that enable the
 * multi-threaded pipeline architecture:
 *
 * 1.  `SampleChunk`: A structure that holds buffers and state for a single
 *     block of I/Q samples as it moves through the processing stages.
 *
 * 2.  `Queue`: A thread-safe, blocking queue used to pass pointers to
 *     `SampleChunk` objects between threads.
 */

#ifndef PIPELINE_TYPES_H_
#define PIPELINE_TYPES_H_

#include "common_types.h" // For complex_float_t, etc.
#include <pthread.h>
#include <stddef.h>

// --- Struct Definitions ---

/**
 * @struct SampleChunk
 * @brief A container for a block of samples and its state as it moves through the pipeline.
 *
 * An array of these structs is allocated at startup to form a memory pool. Pointers
 * to these structs are then passed between threads via queues, avoiding the need
 * for dynamic memory allocation during real-time processing.
 */
typedef struct SampleChunk {
    // --- Buffers ---
    void*            raw_input_data;              ///< Buffer for raw data from the source.
    complex_float_t* complex_pre_resample_data;   ///< Buffer for cf32 data before resampling.
    complex_float_t* complex_resampled_data;      ///< Buffer for cf32 data after resampling.
    complex_float_t* complex_post_resample_data;  ///< Buffer for cf32 data after post-processing.
    complex_float_t* complex_scratch_data;        ///< A general-purpose workspace buffer.
    unsigned char*   final_output_data;           ///< Buffer for the final, converted output data.

    // --- Capacities ---
    size_t raw_input_capacity_bytes;        ///< The max size of the raw_input_data buffer.
    size_t complex_buffer_capacity_samples; ///< The max number of samples for all cf32 buffers.
    size_t final_output_capacity_bytes;     ///< The max size of the final_output_data buffer.

    // --- State Variables ---
    int64_t      frames_read;                 ///< Number of valid frames read from the source.
    unsigned int frames_to_write;             ///< Number of valid frames to be written to the output.
    bool         is_last_chunk;               ///< Flag indicating this is the final chunk in a stream.
    bool         stream_discontinuity_event;  ///< Flag indicating a stream reset (e.g., SDR overrun).
    size_t       input_bytes_per_sample_pair; ///< The size of a single I/Q pair from the source.
} SampleChunk;

/**
 * @struct Queue
 * @brief A standard, blocking, thread-safe queue for passing pointers between threads.
 *
 * This implementation uses a mutex and condition variables to ensure safe access
 * from multiple threads and to allow threads to sleep efficiently while waiting
 * for data to become available or for space to open up.
 */
typedef struct Queue {
    void**          buffer;             ///< The internal ring buffer holding pointers.
    size_t          capacity;           ///< The maximum number of items the queue can hold.
    size_t          count;              ///< The current number of items in the queue.
    size_t          head;               ///< The index of the next item to be dequeued.
    size_t          tail;               ///< The index where the next item will be enqueued.
    pthread_mutex_t mutex;              ///< Mutex to protect access to the queue's state.
    pthread_cond_t  not_empty_cond;     ///< Condition variable to signal when an item is added.
    pthread_cond_t  not_full_cond;      ///< Condition variable to signal when an item is removed.
    bool            shutting_down;      ///< Flag to unblock waiting threads during shutdown.
} Queue;

#endif // PIPELINE_TYPES_H_
