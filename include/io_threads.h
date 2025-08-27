/**
 * @file io_threads.h
 * @brief Declares the entry point functions for the I/O threads.
 *
 * This header provides the function prototypes for the threads responsible for
 * the primary I/O operations of the application:
 *
 * - The Reader Thread: Reads data from the source (file or SDR).
 * - The Writer Thread: Writes processed data to the destination (file or stdout).
 * - The SDR Capture Thread: A dedicated thread for handling SDR hardware callbacks
 *   in buffered mode to maximize stability.
 */

#ifndef IO_THREADS_H_
#define IO_THREADS_H_

// --- Forward Declaration ---
// The thread functions only need a void pointer, but for type safety and
// clarity in documentation, we forward-declare the context struct they will receive.
struct PipelineContext;

// --- Function Declarations ---

/**
 * @brief The reader thread's main function.
 *
 * In real-time/file modes, this function runs the input source's main loop.
 * In buffered SDR mode, it reads framed packets from the SDR input buffer and
 * feeds them into the processing pipeline.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* reader_thread_func(void* arg);

/**
 * @brief The writer thread's main function.
 *
 * This thread is responsible for writing the final processed data to the
 * output file or to standard output. It reads from either the file writer
 * buffer or the stdout queue.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* writer_thread_func(void* arg);

/**
 * @brief The dedicated SDR capture thread's main function (buffered mode only).
 *
 * This thread's only job is to run the SDR hardware's blocking read loop.
 * Its callback function writes framed packets into the shared sdr_input_buffer,
 * decoupling the hardware driver from the main processing pipeline.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* sdr_capture_thread_func(void* arg);


#endif // IO_THREADS_H_
