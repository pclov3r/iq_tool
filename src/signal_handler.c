#include "signal_handler.h"
#include "log.h"
#include "types.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h> // Required for _isatty() and _fileno()
#else
#include <signal.h>
#include <pthread.h>
#include <unistd.h> // Required for isatty() and fileno()
#endif


// --- Add an external declaration for the global console mutex defined in main.c ---
extern pthread_mutex_t g_console_mutex;

// --- Define a sequence to clear the current line on a terminal ---
#define LINE_CLEAR_SEQUENCE "\r \r"

// This static global is the standard way to give a signal/console handler
// access to the application's state.
static AppResources *g_resources_for_signal_handler = NULL;
static volatile sig_atomic_t g_shutdown_flag = 0;


#ifdef _WIN32
// --- WINDOWS IMPLEMENTATION ---
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (!g_shutdown_flag) {
                pthread_mutex_lock(&g_console_mutex);
                if (_isatty(_fileno(stderr))) {
                    fprintf(stderr, LINE_CLEAR_SEQUENCE);
                }
                log_info("Ctrl+C detected, initiating graceful shutdown...");
                pthread_mutex_unlock(&g_console_mutex);
                request_shutdown();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

#else
// --- POSIX (LINUX) IMPLEMENTATION ---
void* signal_handler_thread(void *arg) {
    // This argument is now implicitly used via the global g_resources_for_signal_handler
    (void)arg;
    sigset_t signal_set;
    int sig;

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);

    if (sigwait(&signal_set, &sig) == 0) {
        pthread_mutex_lock(&g_console_mutex);
        if (isatty(fileno(stderr))) {
            fprintf(stderr, LINE_CLEAR_SEQUENCE);
        }
        log_info("Signal %d (%s) received, initiating graceful shutdown...", sig, strsignal(sig));
        pthread_mutex_unlock(&g_console_mutex);
        request_shutdown();
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
    // Check if a shutdown is already in progress to avoid redundant signaling
    if (g_shutdown_flag) {
        return;
    }
    g_shutdown_flag = 1;

    if (g_resources_for_signal_handler) {
        // Signal ALL queues to ensure every thread wakes up
        if (g_resources_for_signal_handler->free_sample_chunk_queue)
            queue_signal_shutdown(g_resources_for_signal_handler->free_sample_chunk_queue);
        if (g_resources_for_signal_handler->raw_to_pre_process_queue)
            queue_signal_shutdown(g_resources_for_signal_handler->raw_to_pre_process_queue);
        if (g_resources_for_signal_handler->pre_process_to_resampler_queue)
            queue_signal_shutdown(g_resources_for_signal_handler->pre_process_to_resampler_queue);
        if (g_resources_for_signal_handler->resampler_to_post_process_queue)
            queue_signal_shutdown(g_resources_for_signal_handler->resampler_to_post_process_queue);
        if (g_resources_for_signal_handler->final_output_queue)
            queue_signal_shutdown(g_resources_for_signal_handler->final_output_queue);
        if (g_resources_for_signal_handler->iq_optimization_data_queue)
            queue_signal_shutdown(g_resources_for_signal_handler->iq_optimization_data_queue);
    }
}

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
void handle_fatal_thread_error(const char* context_msg, AppResources* resources) {
    pthread_mutex_lock(&resources->progress_mutex);
    if (resources->error_occurred) {
        pthread_mutex_unlock(&resources->progress_mutex);
        return; // Error is already being handled by another thread.
    }
    resources->error_occurred = true;
    pthread_mutex_unlock(&resources->progress_mutex);

    log_fatal("%s", context_msg);
    request_shutdown(); // Use the central shutdown mechanism
}
