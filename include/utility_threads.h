/**
 * @file utility_threads.h
 * @brief Declares the implementation functions for asynchronous service threads.
 *
 * These threads run in the background to support the main pipeline but are not
 * part of the direct data flow.
 */

#ifndef UTILITY_THREADS_H_
#define UTILITY_THREADS_H_

/**
 * @brief The main function for the I/Q Optimization utility thread.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* pipeline_thread_iq_optimizer(void* arg);

/**
 * @brief The main function for the SDR Watchdog utility thread.
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* pipeline_thread_watchdog(void* arg);

#endif // UTILITY_THREADS_H_
