// io_threads.h

#ifndef IO_THREADS_H_
#define IO_THREADS_H_

#include "pipeline_context.h" // Needed for the PipelineContext definition

/**
 * @brief The reader thread's main function.
 *        Reads from the input source and places raw data into the pipeline.
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

#endif // IO_THREADS_H_
