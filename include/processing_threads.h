// processing_threads.h

#ifndef PROCESSING_THREADS_H_
#define PROCESSING_THREADS_H_

#include "pipeline_context.h" // Needed for the PipelineContext definition

/**
 * @brief The pre-processor thread's main function.
 *        Handles sample conversion, DC blocking, I/Q correction, and pre-resample frequency shifting.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* pre_processor_thread_func(void* arg);

/**
 * @brief The resampler thread's main function.
 *        Applies anti-aliasing and resamples the I/Q data to the target rate.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* resampler_thread_func(void* arg);

/**
 * @brief The post-processor thread's main function.
 *        Handles user-defined filtering, post-resample frequency shifting, and conversion to the final output format.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* post_processor_thread_func(void* arg);

/**
 * @brief The I/Q optimization thread's main function.
 *        Runs the I/Q imbalance correction algorithm periodically.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* iq_optimization_thread_func(void* arg);

#endif // PROCESSING_THREADS_H_
