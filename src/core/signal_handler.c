/**
 * @file signal_handler.c
 */

#include "core/signal_handler.h"
#include "core/app_context.h" // Provides AppContext
#include "core/module.h"      // Provides ModuleContext
#include "core/queue.h"       // Provides queue_signal_shutdown
#include "core/ring_buffer.h" // Provides ring_buffer_signal_shutdown
#include "core/wait_event.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <strings.h>
#include <unistd.h>
#endif

extern pthread_mutex_t g_console_mutex;

static AppContext *g_resources_for_signal_handler = NULL;
#include <stdatomic.h>
static atomic_bool g_shutdown_flag = ATOMIC_VAR_INIT(false);

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
  switch (dwCtrlType) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    if (!is_shutdown_requested()) {
      // 1. Cosmetic: Force a newline immediately so ^C doesn't mess up the next
      // log
      pthread_mutex_lock(&g_console_mutex);
      if (_isatty(_fileno(stderr))) {
        fprintf(stderr, "\n");
      }
      pthread_mutex_unlock(&g_console_mutex);

      // 2. Trigger shutdown (High Priority)
      // This will trigger the input module's stop_sample_queue_push, which
      // prints logs.
      request_shutdown();

      // 3. Log the event
      log_debug("Ctrl+C detected, initiating graceful shutdown...");
    }
    return TRUE;
  default:
    return FALSE;
  }
}

#else
void *signal_handler_thread(void *arg) {
  (void)arg;
  sigset_t signal_set;
  int sig;

  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGINT);
  sigaddset(&signal_set, SIGTERM);

  // Wait for a signal to arrive
  if (sigwait(&signal_set, &sig) == 0) {
    if (!is_shutdown_requested()) {

      // 1. Cosmetic: Force a newline immediately.
      // This separates the terminal's "^C" echo from the logs that follow.
      pthread_mutex_lock(&g_console_mutex);
      if (isatty(fileno(stderr))) {
        fprintf(stderr, "\n");
      }
      pthread_mutex_unlock(&g_console_mutex);

      // 2. Trigger shutdown (High Priority)
      // This calls the input module's stop_sample_queue_push(), which generates
      // logs. Since we printed \n above, these logs will appear on a fresh
      // line.
      request_shutdown();

      // 3. Log the specific signal
      log_debug("Signal %d (%s) received, initiating graceful shutdown...", sig,
                strsignal(sig));
    }
  }
  return NULL;
}
#endif

void setup_signal_handlers(AppContext *app) {
  g_resources_for_signal_handler = app;
#ifdef _WIN32
  if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
    log_warn("Failed to register console control handler.");
  }
#else
  sigset_t signal_set;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGINT);
  sigaddset(&signal_set, SIGTERM);
  // Block signals in the main thread so they are handled by the dedicated
  // thread
  if (pthread_sigmask(SIG_BLOCK, &signal_set, NULL) != 0) {
    fprintf(stderr, "FATAL: Failed to set signal mask.\n");
    exit(EXIT_FAILURE);
  }
#endif
}

bool is_shutdown_requested(void) {
  return atomic_load_explicit(&g_shutdown_flag, memory_order_relaxed);
}

void reset_shutdown_flag(void) {
  atomic_store_explicit(&g_shutdown_flag, false, memory_order_relaxed);
}

void request_shutdown(void) {
  bool expected = false;
  if (!atomic_compare_exchange_strong(&g_shutdown_flag, &expected, true)) {
    return;
  }

  if (g_resources_for_signal_handler) {
    AppContext *r = g_resources_for_signal_handler;

    // Signal the global shutdown event to wake up any sleeping input threads
    if (r->process_chain.shutdown_event) {
      wait_event_signal(r->process_chain.shutdown_event);
    }

    // Generic shutdown: If the active input module has a stop function, call
    // it. This handles blocking input drivers (like RTL-SDR) and background
    // threads.
    if (r->module.input_api && r->module.input_api->stop_sample_queue_push) {
      ModuleContext context = {.config = r->config, .app = r};
      r->module.input_api->stop_sample_queue_push(&context);
    }

    // Signal all queues to wake up any waiting threads.
    if (r->process_chain.free_sample_chunk_queue)
      queue_signal_shutdown(r->process_chain.free_sample_chunk_queue);

    if (r->process_chain.active_queues) {
      for (int i = 0; i < r->process_chain.num_active_queues; i++) {
        if (r->process_chain.active_queues[i]) {
          queue_signal_shutdown(r->process_chain.active_queues[i]);
        }
      }
    }
    if (r->process_chain.iq_estimation_data_queue)
      queue_signal_shutdown(r->process_chain.iq_estimation_data_queue);

    // Signal all ring buffers to wake up any waiting threads
    if (r->process_chain.source_input_buffer)
      ring_buffer_signal_shutdown(r->process_chain.source_input_buffer);
  }
}

void request_forceful_shutdown(const char *context_msg, AppContext *app) {
  bool expected = false;
  if (atomic_compare_exchange_strong(&app->stats.error_occurred, &expected,
                                     true)) {
    log_fatal("%s", context_msg);
    request_shutdown();
  }
}
