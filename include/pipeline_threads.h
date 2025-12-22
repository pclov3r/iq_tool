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

void* pipeline_thread_sdr_capture(void* arg);
void* pipeline_thread_reader(void* arg);
void* pipeline_thread_pre_processor(void* arg);
void* pipeline_thread_resampler(void* arg);
void* pipeline_thread_post_processor(void* arg);
void* pipeline_thread_writer(void* arg);

#endif // PIPELINE_THREADS_H_
