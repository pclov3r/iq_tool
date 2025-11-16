/**
 * @file pipeline_threads.h
 * @brief PRIVATE: Declares the entry point functions for all pipeline-specific threads.
 *
 * This header provides the function prototypes for all concurrent stages and utility
 * tasks. It is for the internal use of the pipeline.c module only and should not
 * be included by any other part of the application.
 */

#ifndef PIPELINE_THREADS_H_
#define PIPELINE_THREADS_H_

// --- Data Pipeline Stage Functions ---

void* sdr_capture_thread_func(void* arg);
void* reader_thread_func(void* arg);
void* pre_processor_thread_func(void* arg);
void* resampler_thread_func(void* arg);
void* post_processor_thread_func(void* arg);
void* writer_thread_func(void* arg);

#endif // PIPELINE_THREADS_H_
