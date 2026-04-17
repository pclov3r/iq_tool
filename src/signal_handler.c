#include "signal_handler.h"
#include "log.h"
#include "app_context.h"       // Provides AppContext
#include "module.h"            // Provides ModuleContext
#include "queue.h"             // Provides queue_signal_shutdown
#include "ring_buffer.h"       // Provides ring_buffer_signal_shutdown
#include "wait_event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <strings.h>
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
                // 1. Cosmetic: Force a newline immediately so ^C doesn't mess up the next log
                pthread_mutex_lock(&g_console_mutex);
                if (_isatty(_fileno(stderr))) {
                    fprintf(stderr, "\n");
                }
                pthread_mutex_unlock(&g_console_mutex);

                // 2. Trigger shutdown (High Priority)
                // This will trigger rtlsdr_stop_stream, which prints logs.
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
void* signal_handler_thread(void *arg) {
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
            // This calls rtlsdr_stop_stream(), which generates logs.
            // Since we printed \n above, these logs will appear on a fresh line.
            request_shutdown();

            // 3. Log the specific signal
            log_debug("Signal %d (%s) received, initiating graceful shutdown...", sig, strsignal(sig));
        }
    }
    return NULL;
}
#endif

void setup_signal_handlers(AppContext* app) {
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
    // Block signals in the main thread so they are handled by the dedicated thread
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
        AppContext* r = g_resources_for_signal_handler;

        // Signal the global shutdown event to wake up any sleeping input threads
        if (r->pipeline.shutdown_event) {
            wait_event_signal(r->pipeline.shutdown_event);
        }

        // Generic shutdown: If the active input module has a stop function, call it.
        // This handles blocking SDR drivers (like RTL-SDR) and background threads.
        if (r->module.input_api && r->module.input_api->stop_stream) {
            ModuleContext ctx = { .config = r->config, .app = r };
            r->module.input_api->stop_stream(&ctx);
        }

        // Signal all queues to wake up any waiting threads.
        if (r->pipeline.free_sample_chunk_queue)
            queue_signal_shutdown(r->pipeline.free_sample_chunk_queue);
        if (r->pipeline.reader_output_queue)
            queue_signal_shutdown(r->pipeline.reader_output_queue);
        if (r->pipeline.pre_processor_output_queue)
            queue_signal_shutdown(r->pipeline.pre_processor_output_queue);
        if (r->pipeline.resampler_output_queue)
            queue_signal_shutdown(r->pipeline.resampler_output_queue);
        if (r->pipeline.post_processor_output_queue)
            queue_signal_shutdown(r->pipeline.post_processor_output_queue);
        if (r->pipeline.iq_optimization_data_queue)
            queue_signal_shutdown(r->pipeline.iq_optimization_data_queue);
        
        // Signal all ring buffers to wake up any waiting threads
        if (r->pipeline.sdr_input_buffer)
            ring_buffer_signal_shutdown(r->pipeline.sdr_input_buffer);
    }
}

void handle_fatal_thread_error(const char* context_msg, AppContext* app) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&app->stats.error_occurred, &expected, true)) {
        log_fatal("%s", context_msg);
        request_shutdown();
    }
}
