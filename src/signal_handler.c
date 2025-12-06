#include "signal_handler.h"
#include "log.h"
#include "app_context.h"       // Provides AppResources
#include "module.h"            // Provides ModuleContext
#include "queue.h"             // Provides queue_signal_shutdown
#include "ring_buffer.h"       // Provides ring_buffer_signal_shutdown
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

static AppResources *g_resources_for_signal_handler = NULL;
static volatile sig_atomic_t g_shutdown_flag = 0;

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

void setup_signal_handlers(AppResources *resources) {
    g_resources_for_signal_handler = resources;
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
    return g_shutdown_flag != 0;
}

void reset_shutdown_flag(void) {
    g_shutdown_flag = 0;
}

void request_shutdown(void) {
    if (g_shutdown_flag) {
        return;
    }
    g_shutdown_flag = 1;

    if (g_resources_for_signal_handler) {
        AppResources* r = g_resources_for_signal_handler;

        // Generic shutdown: If the active input module has a stop function, call it.
        // This handles blocking SDR drivers (like RTL-SDR) and background threads.
        if (r->selected_input_module_api && r->selected_input_module_api->stop_stream) {
            ModuleContext ctx = { .config = r->config, .resources = r };
            r->selected_input_module_api->stop_stream(&ctx);
        }

        // Signal all queues to wake up any waiting threads.
        if (r->free_sample_chunk_queue)
            queue_signal_shutdown(r->free_sample_chunk_queue);
        if (r->reader_output_queue)
            queue_signal_shutdown(r->reader_output_queue);
        if (r->pre_processor_output_queue)
            queue_signal_shutdown(r->pre_processor_output_queue);
        if (r->resampler_output_queue)
            queue_signal_shutdown(r->resampler_output_queue);
        if (r->post_processor_output_queue)
            queue_signal_shutdown(r->post_processor_output_queue);
        if (r->iq_optimization_data_queue)
            queue_signal_shutdown(r->iq_optimization_data_queue);
        
        // Signal all ring buffers to wake up any waiting threads
        if (r->writer_input_buffer)
            ring_buffer_signal_shutdown(r->writer_input_buffer);
        
        if (r->sdr_input_buffer)
            ring_buffer_signal_shutdown(r->sdr_input_buffer);
    }
}

void handle_fatal_thread_error(const char* context_msg, AppResources* resources) {
    pthread_mutex_lock(&resources->progress_mutex);
    if (resources->error_occurred) {
        pthread_mutex_unlock(&resources->progress_mutex);
        return;
    }
    resources->error_occurred = true;
    pthread_mutex_unlock(&resources->progress_mutex);

    log_fatal("%s", context_msg);
    request_shutdown();
}
