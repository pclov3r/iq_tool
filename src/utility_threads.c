/**
 * @file utility_threads.c
 * @brief Implements the entry-point functions for asynchronous service threads.
 */

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

#include "utility_threads.h"
#include "pipeline_context.h"
#include "constants.h"
#include "app_context.h"
#include "utils.h"
#include "signal_handler.h"
#include "log.h"
#include "iq_correct.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>


/**
 * @brief The I/Q optimization thread's main function.
 *
 * This optional, lower-priority thread periodically runs the I/Q imbalance
 * correction algorithm to refine the correction factors.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* iq_optimization_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->iq_optimization_data_queue)) != NULL) {
        iq_correct_run_optimization(resources, item->complex_sample_buffer_a);
        // Return the chunk to the free pool for reuse
        queue_enqueue(resources->free_sample_chunk_queue, item);
    }
    log_debug("I/Q optimization thread is exiting.");
    return NULL;
}

/**
 * @brief The SDR watchdog thread's main function.
 *
 * This thread periodically checks a heartbeat from the SDR reader to detect
 * deadlocks or driver hangs, forcing a shutdown if the SDR becomes unresponsive.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
void* watchdog_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

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

        double current_time = get_monotonic_time_sec();
        bool timed_out = false;

        pthread_mutex_lock(&resources->progress_mutex);
        double last_heartbeat = resources->last_sdr_heartbeat_time;
        if (last_heartbeat > 0 && (current_time - last_heartbeat) > (WATCHDOG_TIMEOUT_MS / 1000.0)) {
            timed_out = true;
        }
        pthread_mutex_unlock(&resources->progress_mutex);

        if (timed_out) {
            const char* input_device_name = config->input_type_str ? config->input_type_str : "SDR";

            // We use raw fprintf to stderr because the logger might be deadlocked if
            // another thread is holding the console mutex. This is a last-gasp message.
            fprintf(stderr, "\nFATAL: SDR Watchdog triggered.\n");
            fprintf(stderr, "FATAL: No data received from the %s device in over %d seconds.\n",
                      input_device_name, (WATCHDOG_TIMEOUT_MS / 1000));
            fprintf(stderr, "FATAL: The SDR driver has likely hung due to a crash or device removal.\n");
            fprintf(stderr, "FATAL: Forcing application exit.\n");
            fflush(stderr);

            // Terminate the entire process immediately. This is the only correct action
            // for an unrecoverable deadlock.
            exit(EXIT_FAILURE);
        }
    }

    log_debug("SDR watchdog thread is exiting.");
    return NULL;
}
