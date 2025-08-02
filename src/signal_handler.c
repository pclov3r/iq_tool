#include "signal_handler.h"
#include "log.h"
#include "types.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>      // Required for _isatty() and _fileno()
#else
#include <signal.h>
#include <pthread.h>
#include <unistd.h>  // Required for isatty() and fileno()
#endif

// --- FIX: Add an external declaration for the global console mutex defined in main.c ---
extern pthread_mutex_t g_console_mutex;

// --- FIX: Define a sequence to clear the current line on a terminal ---
#define LINE_CLEAR_SEQUENCE "\r                                                                                \r"

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
                // --- FIX: Lock the console for the entire atomic operation ---
                pthread_mutex_lock(&g_console_mutex);
                if (_isatty(_fileno(stderr))) {
                    fprintf(stderr, LINE_CLEAR_SEQUENCE);
                }
                log_info("Ctrl+C detected, initiating graceful shutdown...");
                pthread_mutex_unlock(&g_console_mutex);

                g_shutdown_flag = 1;

                if (g_resources_for_signal_handler) {
                    if (g_resources_for_signal_handler->input_q) queue_signal_shutdown(g_resources_for_signal_handler->input_q);
                    if (g_resources_for_signal_handler->output_q) queue_signal_shutdown(g_resources_for_signal_handler->output_q);
                    if (g_resources_for_signal_handler->free_pool_q) queue_signal_shutdown(g_resources_for_signal_handler->free_pool_q);
                }
            }
            return TRUE;
        default:
            return FALSE;
    }
}

#else
// --- POSIX (LINUX) IMPLEMENTATION ---

void* signal_handler_thread(void *arg) {
    AppResources *resources = (AppResources*)arg;
    sigset_t signal_set;
    int sig;

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);

    if (sigwait(&signal_set, &sig) == 0) {
        // --- FIX: Lock the console for the entire atomic operation ---
        pthread_mutex_lock(&g_console_mutex);
        if (isatty(fileno(stderr))) {
            fprintf(stderr, LINE_CLEAR_SEQUENCE);
        }
        log_info("Signal %d (%s) received, initiating graceful shutdown...", sig, strsignal(sig));
        pthread_mutex_unlock(&g_console_mutex);

        g_shutdown_flag = 1;

        if (resources) {
            if (resources->input_q) queue_signal_shutdown(resources->input_q);
            if (resources->output_q) queue_signal_shutdown(resources->output_q);
            if (resources->free_pool_q) queue_signal_shutdown(resources->free_pool_q);
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
