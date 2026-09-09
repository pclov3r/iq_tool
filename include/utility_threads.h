/**
 * @file utility_threads.h
 * @brief Declares the implementation functions for asynchronous service
 * threads.
 *
 * These threads run in the background to support the main process_chain but are
 * not part of the direct data flow.
 */

#ifndef UTILITY_THREADS_H_
#define UTILITY_THREADS_H_

/**
 * @brief The main function for the Source Watchdog utility thread.
 * @param arg A void pointer to the ProcessChainContext struct.
 * @return NULL.
 */
void *process_chain_thread_watchdog(void *arg);

#endif // UTILITY_THREADS_H_
