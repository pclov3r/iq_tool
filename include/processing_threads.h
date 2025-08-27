/**
 * @file processing_threads.h
 * @brief Declares the entry point functions for the core DSP threads.
 *
 * This header provides the function prototypes for the threads that form the
 * main stages of the digital signal processing pipeline.
 */

#ifndef PROCESSING_THREADS_H_
#define PROCESSING_THREADS_H_

// --- Forward Declaration ---
// The thread functions only need a void pointer, but for type safety and
// clarity in documentation, we forward-declare the context struct they will receive.
struct PipelineContext;

// --- Function Declarations ---

/**
 * @brief The pre-processor thread's main function.
 *
 * This thread is responsible for the first stage of DSP. It takes raw samples
 * from the reader, converts them to complex floats, and applies initial
 * processing such as DC blocking, I/Q correction, and any pre-resample
 * filtering or frequency shifting.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* pre_processor_thread_func(void* arg);

/**
 * @brief The resampler thread's main function.
 *
 * This thread's sole responsibility is to change the sample rate of the I/Q
 * stream using a high-quality polyphase filter bank resampler.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* resampler_thread_func(void* arg);

/**
 * @brief The post-processor thread's main function.
 *
 * This thread is responsible for the final stage of DSP. It takes resampled
 * data and applies any post-resample filtering or frequency shifting, then
 * converts the complex float samples into the final, integer-based output format.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* post_processor_thread_func(void* arg);

/**
 * @brief The I/Q optimization thread's main function.
 *
 * This optional, lower-priority thread periodically runs the I/Q imbalance
 * correction algorithm on captured data to refine the correction factors.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* iq_optimization_thread_func(void* arg);

#endif // PROCESSING_THREADS_H_
