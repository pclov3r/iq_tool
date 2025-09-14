/**
 * @file thread_functions.h
 * @brief Declares the entry point functions for all application data pipeline threads.
 *
 * This header provides the function prototypes for the threads that form the
 * core of the application's processing pipeline, as well as utility threads
 * like the I/Q optimizer.
 */

#ifndef THREAD_FUNCTIONS_H_
#define THREAD_FUNCTIONS_H_

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
 * output file or to standard output.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* writer_thread_func(void* arg);

/**
 * @brief The dedicated SDR capture thread's main function (buffered mode only).
 *
 * This thread's only job is to run the SDR hardware's blocking read loop,
 * writing framed packets into the shared sdr_input_buffer.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* sdr_capture_thread_func(void* arg);

/**
 * @brief The pre-processor thread's main function.
 *
 * This thread performs the first stage of DSP, including sample format
 * conversion, DC blocking, I/Q correction, and pre-resample filtering.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* pre_processor_thread_func(void* arg);

/**
 * @brief The resampler thread's main function.
 *
 * This thread changes the sample rate of the I/Q stream.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* resampler_thread_func(void* arg);

/**
 * @brief The post-processor thread's main function.
 *
 * This thread performs the final stage of DSP, including post-resample
 * filtering and conversion to the final output format.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* post_processor_thread_func(void* arg);

/**
 * @brief The I/Q optimization thread's main function.
 *
 * This optional, lower-priority thread periodically runs the I/Q imbalance
 * correction algorithm to refine the correction factors.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* iq_optimization_thread_func(void* arg);


#endif // THREAD_FUNCTIONS_H_
