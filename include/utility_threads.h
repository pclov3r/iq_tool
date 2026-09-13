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

#include <stdatomic.h>
#include <stdbool.h>

/**
 * @struct SdrInitWatchdogContext
 * @brief Context passed to the SDR initialization watchdog thread.
 */
typedef struct SdrInitWatchdogContext {
  const char *module_name;  ///< Name of the input module being initialized.
  atomic_bool is_complete;  ///< Flag set by the caller when initialization completes.
} SdrInitWatchdogContext;

/**
 * @brief Watchdog thread that forces application exit if SDR initialization hangs.
 * @param arg A pointer to the SdrInitWatchdogContext struct.
 * @return NULL.
 */
void *sdr_init_watchdog_thread(void *arg);

/**
 * @brief The main function for the Input Watchdog utility thread.
 * @param arg A void pointer to the ProcessChainContext struct.
 * @return NULL.
 */
void *process_chain_thread_watchdog(void *arg);

#endif // UTILITY_THREADS_H_
