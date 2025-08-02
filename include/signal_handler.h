#ifndef SIGNAL_HANDLER_H_
#define SIGNAL_HANDLER_H_

#include <stdbool.h>
#include "types.h" // Required for AppResources type

/**
 * @brief Sets up the application's signal handlers.
 * This is platform-aware and will use the correct mechanism for the OS.
 * @param resources A pointer to the main AppResources struct, which is needed
 *                  by the handler to perform shutdown actions.
 */
void setup_signal_handlers(AppResources *resources);

/**
 * @brief The dedicated signal handling thread function (POSIX-only).
 *
 * This function should be run in its own thread on Linux/POSIX systems.
 * @param arg A void pointer to the AppResources struct.
 * @return NULL.
 */
void* signal_handler_thread(void *arg);

/**
 * @brief Checks if a shutdown has been requested via a signal.
 * @return true if a shutdown signal has been received, false otherwise.
 */
bool is_shutdown_requested(void);

/**
 * @brief Resets the internal shutdown flag.
 * This should be called if the application's main function is re-entered
 * without a full process restart (e.g., in some testing environments).
 */
void reset_shutdown_flag(void);

#endif // SIGNAL_HANDLER_H_
