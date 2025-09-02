#include "signal_handler.h"
#include "log.h"
#include "app_context.h"       // Provides AppResources
#include "input_source.h"      // Provides InputSourceContext
#include "queue.h"             // Provides queue_signal_shutdown
#include "file_write_buffer.h" // Provides file_write_buffer_signal_shutdown
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

#define LINE_CLEAR_SEQUENCE "\r \r"

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
                pthread_mutex_lock(&g_console_mutex);
                if (_isatty(_fileno(stderr))) {
                    fprintf(stderr, LINE_CLEAR_SEQUENCE);
                }
                log_debug("Ctrl+C detected, initiating graceful shutdown...");
                pthread_mutex_unlock(&g_console_mutex);
                request_shutdown();
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

    if (sigwait(&signal_set, &sig) == 0) {
        if (!is_shutdown_requested()) {
            pthread_mutex_lock(&g_console_mutex);
            if (isatty(fileno(stderr))) {
                fprintf(stderr, LINE_CLEAR_SEQUENCE);
            }
            log_debug("Signal %d (%s) received, initiating graceful shutdown...", sig, strsignal(sig));
            pthread_mutex_unlock(&g_console_mutex);
            request_shutdown();
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

        // Special case for RTL-SDR to unblock its synchronous read loop
        if (r->config && r->config->input_type_str && strcasecmp(r->config->input_type_str, "rtlsdr") == 0) {
            if (r->selected_input_ops && r->selected_input_ops->stop_stream) {
                log_debug("Signal handler is calling stop_stream for RTL-SDR to unblock reader thread.");
                InputSourceContext ctx = { .config = r->config, .resources = r };
                r->selected_input_ops->stop_stream(&ctx);
            }
        }

        // Signal all queues to wake up any waiting threads
        if (r->free_sample_chunk_queue)
            queue_signal_shutdown(r->free_sample_chunk_queue);
        if (r->raw_to_pre_process_queue)
            queue_signal_shutdown(r->raw_to_pre_process_queue);
        if (r->pre_process_to_resampler_queue)
            queue_signal_shutdown(r->pre_process_to_resampler_queue);
        if (r->resampler_to_post_process_queue)
            queue_signal_shutdown(r->resampler_to_post_process_queue);
        if (r->stdout_queue)
            queue_signal_shutdown(r->stdout_queue);
        if (r->iq_optimization_data_queue)
            queue_signal_shutdown(r->iq_optimization_data_queue);
        
        // Signal all ring buffers to wake up any waiting threads
        if (r->file_write_buffer)
            file_write_buffer_signal_shutdown(r->file_write_buffer);
        
        // Also signal the SDR input buffer to unblock the reader thread in buffered mode.
        if (r->sdr_input_buffer)
            file_write_buffer_signal_shutdown(r->sdr_input_buffer);
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
