/**
 * @file signal_handler.h
 * @brief Defines the interface for handling process signals (e.g., Ctrl+C) gracefully.
 *
 * This module provides a platform-independent way to catch signals like SIGINT
 * and SIGTERM. When a signal is caught, it initiates a graceful shutdown of the
 * application by setting a global flag and signaling all blocking components
 * (queues, buffers) to wake up and terminate.
 */

#ifndef SIGNAL_HANDLER_H_
#define SIGNAL_HANDLER_H_

#include <stdbool.h>
#include "app_context.h" // Provides the full definition for AppResources

// --- Function Declarations ---

/**
 * @brief Sets up the application's signal handlers.
 *
 * This is platform-aware and will use the correct mechanism for the OS
 * (SetConsoleCtrlHandler on Windows, sigwait in a dedicated thread on POSIX).
 *
 * @param resources A pointer to the main AppResources struct, which is needed
 *                  by the handler to perform shutdown actions on queues and buffers.
 */
void setup_signal_handlers(AppResources *resources);

/**
 * @brief The dedicated signal handling thread function (POSIX-only).
 *
 * This function should be run in its own detached thread on Linux/POSIX systems.
 * It blocks waiting for a signal and then initiates the shutdown.
 *
 * @param arg A void pointer (unused, but required by pthread_create).
 * @return NULL.
 */
void* signal_handler_thread(void *arg);

/**
 * @brief Checks if a shutdown has been requested via a signal.
 *
 * This function provides a thread-safe way to check the global shutdown flag.
 *
 * @return true if a shutdown signal has been received, false otherwise.
 */
bool is_shutdown_requested(void);

/**
 * @brief Resets the internal shutdown flag to its initial state.
 */
void reset_shutdown_flag(void);

/**
 * @brief Programmatically requests a graceful shutdown.
 *
 * This function sets the internal shutdown flag and signals all queues and
 * buffers, mimicking the behavior of a Ctrl+C event. It is thread-safe and
 * can be called from any thread to terminate the application.
 */
void request_shutdown(void);

/**
 * @brief Handles a fatal error that occurs within any processing thread.
 *
 * This is the central, thread-safe function for reporting a fatal error.
 * It ensures the error is logged exactly once, a global error flag is set,
 * and a graceful shutdown is initiated via request_shutdown().
 *
 * @param context_msg A descriptive error message string.
 * @param resources A pointer to the main AppResources struct.
 */
void handle_fatal_thread_error(const char* context_msg, AppResources* resources);


#endif // SIGNAL_HANDLER_H_
