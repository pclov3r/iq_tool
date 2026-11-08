/**
 * @file utility_threads.c
 * @brief Implements the entry-point functions for asynchronous service threads.
 */

#include "utility_threads.h"
#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "platform.h"
#include "signal_handler.h"
#include "thread_manager.h"
#include "wait_event.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * The SDR initialization watchdog thread monitors device initialization and
 * forces an immediate application exit if the hardware driver deadlocks or
 * hangs.
 */
void *sdr_init_watchdog_thread(void *arg) {
  SdrInitWatchdogContext *ctx = (SdrInitWatchdogContext *)arg;
  if (!ctx || !ctx->completion_event)
    return NULL;

  if (wait_event_wait_timeout(ctx->completion_event,
                              SDR_INITIALIZE_TIMEOUT_MS) ==
      WAIT_EVENT_RESULT_TIMEOUT) {
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
  return NULL;
}

/*
 * The Input watchdog thread periodically checks a heartbeat from the input
 * reader to detect deadlocks or driver hangs, forcing a shutdown if the input
 * becomes unresponsive.
 */
static void *input_watchdog_thread(void *arg) {
  AppContext *app = (AppContext *)arg;
  const AppConfig *config = app ? app->config : NULL;

  // Give the SDR a moment to start up before we start checking
  if (wait_event_wait_timeout(app->process_chain.shutdown_event,
                              WATCHDOG_TIMEOUT_MS) !=
      WAIT_EVENT_RESULT_TIMEOUT) {
    log_debug("Input watchdog thread is exiting.");
    return NULL;
  }

  while (!is_shutdown_requested()) {
    if (wait_event_wait_timeout(app->process_chain.shutdown_event,
                                WATCHDOG_INTERVAL_MS) !=
        WAIT_EVENT_RESULT_TIMEOUT) {
      break;
    }

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
          (config && config->input_module.name) ? config->input_module.name
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

void setup_input_watchdog(AppContext *app) {
  if (app && app->process_chain_mode == PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH) {
    input_update_heartbeat(app);
    thread_manager_spawn(&app->thread_manager, "watchdog", PRIORITY_NORMAL,
                         input_watchdog_thread, app);
  }
}
