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
#include "iq_correction.h"
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
void* pipeline_thread_iq_optimizer(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(app->pipeline.iq_optimization_data_queue)) != NULL) {
        iq_correction_run_optimization(&app->dsp, item->complex_sample_buffer_a);
        // Return the chunk to the free pool for reuse
        queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
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
void* pipeline_thread_watchdog(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;
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

        double current_time = utils_get_time();
        bool timed_out = false;

        pthread_mutex_lock(&app->stats.mutex);
        double last_heartbeat = app->stats.last_sdr_heartbeat_time;
        if (last_heartbeat > 0 && (current_time - last_heartbeat) > (WATCHDOG_TIMEOUT_MS / 1000.0)) {
            timed_out = true;
        }
        pthread_mutex_unlock(&app->stats.mutex);

        if (timed_out) {
            const char* input_device_name = config->input.type_name ? config->input.type_name : "SDR";

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
