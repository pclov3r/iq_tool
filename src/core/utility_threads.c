/**
 * @file utility_threads.c
 * @brief Implements the entry-point functions for asynchronous service threads.
 */

#include "utility_threads.h"
#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "platform.h"
#include "process_chain_context.h"
#include "queue.h"
#include "signal_handler.h"
#include "utilities.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * The SDR initialization watchdog thread monitors device initialization and
 * forces an immediate application exit if the hardware driver deadlocks or
 * hangs.
 */
void *sdr_init_watchdog_thread(void *arg) {
  SdrInitWatchdogContext *ctx = (SdrInitWatchdogContext *)arg;
  const int interval_ms = 50;
  int elapsed_ms = 0;

  while (!atomic_load_explicit(&ctx->is_complete, memory_order_relaxed)) {
    platform_sleep(interval_ms);
    elapsed_ms += interval_ms;
    if (elapsed_ms >= SDR_INITIALIZE_TIMEOUT_MS) {
      if (!atomic_load_explicit(&ctx->is_complete, memory_order_relaxed)) {
        char fatal_message[256];
        int len = snprintf(
            fatal_message, sizeof(fatal_message),
            "\nFATAL: Input Watchdog triggered.\n"
            "FATAL: No response from '%s' module within %d seconds.\n"
            "FATAL: The input module/driver has likely hung due to a crash or "
            "device removal.\n"
            "FATAL: Forcing application exit.\n",
            ctx->module_name ? ctx->module_name : "input",
            SDR_INITIALIZE_TIMEOUT_MS / 1000);

        if (len > 0) {
          platform_write_stderr(fatal_message, (size_t)len);
        }
        _exit(EXIT_FAILURE);
      }
      break;
    }
  }
  return NULL;
}

/*
 * The Input watchdog thread periodically checks a heartbeat from the input
 * reader to detect deadlocks or driver hangs, forcing a shutdown if the input
 * becomes unresponsive.
 */
void *process_chain_thread_watchdog(void *arg) {
  ProcessChainContext *args = (ProcessChainContext *)arg;
  AppContext *app = args->app;
  AppConfig *config = args->config;

  // Give the SDR a moment to start up before we start checking
  platform_sleep(WATCHDOG_TIMEOUT_MS);

  while (!is_shutdown_requested()) {
    platform_sleep(WATCHDOG_INTERVAL_MS);

    double current_time = platform_get_time();
    bool timed_out = false;

    double last_heartbeat = atomic_load_explicit(
        &app->stats.last_input_heartbeat_time, memory_order_relaxed);
    if (last_heartbeat > 0 &&
        (current_time - last_heartbeat) > (WATCHDOG_TIMEOUT_MS / 1000.0)) {
      timed_out = true;
    }

    if (timed_out) {
      char fatal_message[256];
      int len = snprintf(
          fatal_message, sizeof(fatal_message),
          "\nFATAL: Input Watchdog triggered.\n"
          "FATAL: No response from '%s' module within %d seconds.\n"
          "FATAL: The input module/driver has likely hung due to a crash or "
          "device removal.\n"
          "FATAL: Forcing application exit.\n",
          (config && config->input.type_name) ? config->input.type_name
                                              : "input",
          WATCHDOG_TIMEOUT_MS / 1000);

      if (len > 0) {
        platform_write_stderr(fatal_message, (size_t)len);
      }

      // Terminate the entire process immediately. This is the only correct
      // action for an unrecoverable deadlock.
      _exit(EXIT_FAILURE);
    }
  }

  log_debug("Input watchdog thread is exiting.");
  return NULL;
}
