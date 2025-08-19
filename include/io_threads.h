// io_threads.h

#ifndef IO_THREADS_H_
#define IO_THREADS_H_

#include "pipeline_context.h" // Needed for the PipelineContext definition

/**
 * @brief The reader thread's main function.
 *        In real-time/file modes, it runs the input source's main loop.
 *        In buffered SDR mode, it reads from the input buffer and feeds the pipeline.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* reader_thread_func(void* arg);

/**
 * @brief The writer thread's main function.
 *        Writes the final processed data to the output file or stdout.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* writer_thread_func(void* arg);

/**
 * @brief The dedicated SDR capture thread's main function (buffered mode only).
 *        This thread's only job is to run the SDR hardware's blocking read loop,
 *        allowing its callback to write samples into the sdr_input_buffer.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* sdr_capture_thread_func(void* arg);


#endif // IO_THREADS_H_
