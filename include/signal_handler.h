// signal_handler.h

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

/**
 * @brief Programmatically requests a graceful shutdown.
 *
 * This function sets the internal shutdown flag and signals the queues,
 * mimicking the behavior of a Ctrl+C event. It is thread-safe.
 */
void request_shutdown(void);

/**
 * @brief Handles a fatal error that occurs within a thread.
 *
 * This is the central, thread-safe function for reporting a fatal error.
 * It ensures the error is logged, a global error flag is set, and a
 * graceful shutdown is initiated via request_shutdown().
 *
 * @param context_msg A descriptive error message string.
 * @param resources A pointer to the main AppResources struct.
 */
void handle_fatal_thread_error(const char* context_msg, AppResources* resources);


#endif // SIGNAL_HANDLER_H_
