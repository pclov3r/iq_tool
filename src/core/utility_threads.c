/**
 * @file utility_threads.c
 * @brief Implements the entry-point functions for asynchronous service threads.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "core/app_context.h"
#include "core/constants.h"
#include "core/process_chain_context.h"
#include "core/queue.h"
#include "core/signal_handler.h"
#include "core/utilities.h"
#include "core/utility_threads.h"
#include "dsp/iq_correction.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief The I/Q optimization thread's main function.
 *
 * This optional, lower-priority thread periodically runs the I/Q imbalance
 * correction algorithm to refine the correction factors.
 *
 * @param arg A void pointer to the ProcessChainContext struct.
 * @return NULL.
 */
#include "core/process_chain_manager.h" // For process_chain_get_module_state

void *process_chain_thread_iq_estimator(void *arg) {
  ProcessChainContext *args = (ProcessChainContext *)arg;
  AppContext *app = args->app;

  void *iq_state = process_chain_get_module_state(app, "iq_correct");

  void *buffer;
  while ((buffer = queue_dequeue(
              app->process_chain.iq_estimation_data_queue)) != NULL) {
    if (iq_state) {
      iq_correction_run_estimation(iq_state, (ComplexFloat *)buffer);
    }
    // Return the chunk to the free pool for reuse
    queue_enqueue(app->process_chain.iq_estimation_free_queue, buffer);
  }
  log_debug("I/Q optimization thread is exiting.");
  return NULL;
}

/**
 * @brief The Source watchdog thread's main function.
 *
 * This thread periodically checks a heartbeat from the source reader to detect
 * deadlocks or driver hangs, forcing a shutdown if the source becomes
 * unresponsive.
 *
 * @param arg A void pointer to the ProcessChainContext struct.
 * @return NULL.
 */
void *process_chain_thread_watchdog(void *arg) {
  ProcessChainContext *args = (ProcessChainContext *)arg;
  AppContext *app = args->app;
  AppConfig *config = args->config;
  (void)config;

  // Give the SDR a moment to start up before we start checking
#ifdef _WIN32
  Sleep(WATCHDOG_TIMEOUT_MS);
#else
  sleep(WATCHDOG_TIMEOUT_MS / 1000);
#endif

  while (!is_shutdown_requested()) {
#ifdef _WIN32
    Sleep(WATCHDOG_INTERVAL_MS);
#else
    sleep(WATCHDOG_INTERVAL_MS / 1000);
#endif

    double current_time = utility_get_time();
    bool timed_out = false;

    double last_heartbeat = atomic_load_explicit(
        &app->stats.last_source_heartbeat_time, memory_order_relaxed);
    if (last_heartbeat > 0 &&
        (current_time - last_heartbeat) > (WATCHDOG_TIMEOUT_MS / 1000.0)) {
      timed_out = true;
    }

    if (timed_out) {
#ifndef _WIN32
      const char fatal_message[] = "\nFATAL: Source Watchdog triggered.\n"
                                   "FATAL: No data received from device.\n"
                                   "FATAL: The input driver has likely hung "
                                   "due to a crash or device removal.\n"
                                   "FATAL: Forcing application exit.\n";
      write(STDERR_FILENO, fatal_message, sizeof(fatal_message) - 1);
#else
      HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
      DWORD written;
      const char *fatal_message = "\nFATAL: Source Watchdog triggered.\n"
                                  "FATAL: No data received from device.\n"
                                  "FATAL: The input driver has likely hung due "
                                  "to a crash or device removal.\n"
                                  "FATAL: Forcing application exit.\n";
      WriteFile(hStdErr, fatal_message, (DWORD)strlen(fatal_message), &written,
                NULL);
#endif

      // Terminate the entire process immediately. This is the only correct
      // action for an unrecoverable deadlock.
      _exit(EXIT_FAILURE);
    }
  }

  log_debug("Source watchdog thread is exiting.");
  return NULL;
}
