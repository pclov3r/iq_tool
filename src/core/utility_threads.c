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

#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "process_chain_context.h"
#include "queue.h"
#include "signal_handler.h"
#include "utilities.h"
#include "utility_threads.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * The SDR initialization watchdog thread monitors device initialization and
 * forces an immediate application exit if the hardware driver deadlocks or hangs.
 */
void *sdr_init_watchdog_thread(void *arg) {
  SdrInitWatchdogContext *ctx = (SdrInitWatchdogContext *)arg;
  const int interval_ms = 50;
  int elapsed_ms = 0;

  while (!atomic_load_explicit(&ctx->is_complete, memory_order_relaxed)) {
#ifdef _WIN32
    Sleep(interval_ms);
#else
    struct timespec ts = {0, interval_ms * 1000000L};
    nanosleep(&ts, NULL);
#endif
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

#ifndef _WIN32
        if (len > 0) {
          write(STDERR_FILENO, fatal_message, (size_t)len);
        }
#else
        HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD written;
        if (len > 0) {
          WriteFile(hStdErr, fatal_message, (DWORD)len, &written, NULL);
        }
#endif
        _exit(EXIT_FAILURE);
      }
      break;
    }
  }
  return NULL;
}

/*
 * The Input watchdog thread periodically checks a heartbeat from the source
 * reader to detect deadlocks or driver hangs, forcing a shutdown if the source
 * becomes unresponsive.
 */
void *process_chain_thread_watchdog(void *arg) {
  ProcessChainContext *args = (ProcessChainContext *)arg;
  AppContext *app = args->app;
  AppConfig *config = args->config;

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

#ifndef _WIN32
      if (len > 0) {
        write(STDERR_FILENO, fatal_message, (size_t)len);
      }
#else
      HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
      DWORD written;
      if (len > 0) {
        WriteFile(hStdErr, fatal_message, (DWORD)len, &written, NULL);
      }
#endif

      // Terminate the entire process immediately. This is the only correct
      // action for an unrecoverable deadlock.
      _exit(EXIT_FAILURE);
    }
  }

  log_debug("Input watchdog thread is exiting.");
  return NULL;
}
