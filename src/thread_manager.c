/**
 * @file thread_manager.c
 * @brief Implements the management of all processing threads and their lifecycle.
 */

#include "thread_manager.h"
#include "thread_functions.h" // For the DATA pipeline thread entry-point functions
#include "pipeline_context.h"
#include "log.h"
#include "utils.h"
#include "signal_handler.h"
#include "input_manager.h"    // Needed for is_sdr_input()
#include "queue.h"            // Needed for queue_init() and queue_destroy()
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

// --- Private Helper Struct ---

// A local struct to map a thread's name to its function and creation flag.
typedef struct {
    const char* name;
    void* (*func)(void*);
    bool is_required;
} ThreadStarter;


// --- Private Watchdog Thread Function ---

/**
 * @brief A watchdog thread that monitors the SDR reader thread for deadlocks.
 *
 * This thread periodically checks a 'heartbeat' timestamp that the SDR reader
 * thread is expected to update. If the timestamp becomes too old, this
 * watchdog assumes the SDR thread is hung and forces the application to exit.
 *
 * @param arg A void pointer to the PipelineContext struct.
 * @return NULL.
 */
static void* watchdog_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    // Give the SDR a moment to start up before we start checking
#ifdef _WIN32
    Sleep(WATCHDOG_TIMEOUT_MS);
#else
    // POSIX sleep takes seconds, so we convert from our MS standard.
    sleep(WATCHDOG_TIMEOUT_MS / 1000);
#endif

    while (!is_shutdown_requested()) {
#ifdef _WIN32
        Sleep(WATCHDOG_INTERVAL_MS);
#else
        // POSIX sleep takes seconds, so we convert from our MS standard.
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

            // Terminate the entire process immediately. This is the only correct action
            // for an unrecoverable deadlock.
            exit(EXIT_FAILURE);
        }
    }

    log_debug("SDR watchdog thread is exiting.");
    return NULL;
}


// --- Public Function Implementations ---

bool threads_init(ThreadManager* manager, struct AppConfig* config, struct AppResources* resources, struct PipelineContext* pipeline_context) {
    if (!manager || !config || !resources || !pipeline_context) {
        return false;
    }

    // Store references to the main application contexts
    manager->config = config;
    manager->resources = resources;
    manager->pipeline_context = pipeline_context;
    manager->num_threads_started = 0;

    // --- Dynamic Pipeline Logic: Step 1 - Determine which threads are needed ---
    memset(&manager->threads_to_create, 0, sizeof(ThreadFlags));
    ThreadFlags* flags = &manager->threads_to_create;

    flags->reader = true;
    flags->writer = true;
    flags->sdr_capture = (resources->pipeline_mode == PIPELINE_MODE_BUFFERED_SDR);
    flags->sdr_watchdog = is_sdr_input(config->input_type_str, &resources->setup_arena);
    flags->iq_optimizer = config->iq_correction.enable;

    if (config->raw_passthrough) {
        log_debug("Dynamic Pipeline: Configuring for raw passthrough (Reader -> Writer).");
        flags->pre_processor = false;
        flags->resampler = false;
        flags->post_processor = false;
    } else {
        // Pre and post-processing are always needed if not in passthrough mode.
        flags->pre_processor = true;
        flags->post_processor = true;
        if (config->no_resample) {
            log_debug("Dynamic Pipeline: Configuring for native rate processing (No Resampler).");
            flags->resampler = false;
        } else {
            log_debug("Dynamic Pipeline: Configuring for full DSP chain.");
            flags->resampler = true;
        }
    }
    // Copy the final decision to the main resources struct for other parts of the app to see.
    memcpy(&resources->threads_to_create, flags, sizeof(ThreadFlags));


    // --- Dynamic Pipeline Logic: Step 2 - Create and wire up the queues ---
    MemoryArena* arena = &resources->setup_arena;
    Queue* last_output_queue = NULL;

    // The Reader is always the start of the processing chain.
    resources->reader_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!resources->reader_output_queue || !queue_init(resources->reader_output_queue, PIPELINE_NUM_CHUNKS, arena)) return false;
    last_output_queue = resources->reader_output_queue;

    // Conditionally wire up the Pre-Processor
    if (flags->pre_processor) {
        resources->pre_processor_input_queue = last_output_queue;
        resources->pre_processor_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!resources->pre_processor_output_queue || !queue_init(resources->pre_processor_output_queue, PIPELINE_NUM_CHUNKS, arena)) return false;
        last_output_queue = resources->pre_processor_output_queue;
    }

    // Conditionally wire up the Resampler
    if (flags->resampler) {
        resources->resampler_input_queue = last_output_queue;
        resources->resampler_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!resources->resampler_output_queue || !queue_init(resources->resampler_output_queue, PIPELINE_NUM_CHUNKS, arena)) return false;
        last_output_queue = resources->resampler_output_queue;
    }

    // Conditionally wire up the Post-Processor
    if (flags->post_processor) {
        resources->post_processor_input_queue = last_output_queue;
        resources->post_processor_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!resources->post_processor_output_queue || !queue_init(resources->post_processor_output_queue, PIPELINE_NUM_CHUNKS, arena)) return false;
        last_output_queue = resources->post_processor_output_queue;
    }

    // The writer is always the end of the chain.
    resources->writer_input_queue = last_output_queue;

    return true;
}

bool threads_start_all(ThreadManager* manager) {
    if (!manager) return false;

    // Define the complete list of all possible threads and their creation flags.
    const ThreadStarter threads_to_start[MAX_PIPELINE_THREADS] = {
        { "SDR Capture",   sdr_capture_thread_func,   manager->threads_to_create.sdr_capture },
        { "Reader",        reader_thread_func,        manager->threads_to_create.reader },
        { "Pre-Processor", pre_processor_thread_func, manager->threads_to_create.pre_processor },
        { "Resampler",     resampler_thread_func,     manager->threads_to_create.resampler },
        { "Post-Processor",post_processor_thread_func,manager->threads_to_create.post_processor },
        { "Writer",        writer_thread_func,        manager->threads_to_create.writer },
        { "I/Q Optimizer", iq_optimization_thread_func, manager->threads_to_create.iq_optimizer },
        { "SDR Watchdog",  watchdog_thread_func,      manager->threads_to_create.sdr_watchdog },
    };

    log_debug("Starting processing threads...");

    // Initialize the SDR heartbeat time before starting threads.
    pthread_mutex_lock(&manager->resources->progress_mutex);
    manager->resources->last_sdr_heartbeat_time = get_monotonic_time_sec();
    pthread_mutex_unlock(&manager->resources->progress_mutex);

    // Loop through the list and create only the required threads.
    for (int i = 0; i < MAX_PIPELINE_THREADS; ++i) {
        if (threads_to_start[i].is_required) {
            if (pthread_create(&manager->thread_handles[manager->num_threads_started], NULL, threads_to_start[i].func, manager->pipeline_context) != 0) {
                log_fatal("Failed to create %s thread: %s", threads_to_start[i].name, strerror(errno));
                // A thread failed to create. Signal all already-running threads to shut down.
                request_shutdown();
                return false; // Return failure to stop the process.
            }
            log_debug("Thread '%s' created successfully.", threads_to_start[i].name);
            // Only increment the counter on success.
            manager->num_threads_started++;
        }
    }

    return true;
}

void threads_join_all(ThreadManager* manager) {
    if (!manager || manager->num_threads_started == 0) {
        return;
    }

    log_debug("Waiting for %d processing thread(s) to complete...", manager->num_threads_started);
    for (int i = 0; i < manager->num_threads_started; i++) {
        if (pthread_join(manager->thread_handles[i], NULL) != 0) {
            log_warn("Error joining thread %d.", i);
        }
    }
    log_debug("All processing threads have joined successfully.");
}


void threads_destroy_queues(struct AppResources* resources) {
    if (!resources) return;

    // Safely destroy all possible queues by checking for NULL
    if(resources->free_sample_chunk_queue) queue_destroy(resources->free_sample_chunk_queue);
    if(resources->reader_output_queue) queue_destroy(resources->reader_output_queue);
    if(resources->pre_processor_output_queue) queue_destroy(resources->pre_processor_output_queue);
    if(resources->resampler_output_queue) queue_destroy(resources->resampler_output_queue);
    if(resources->post_processor_output_queue) queue_destroy(resources->post_processor_output_queue);
    if(resources->iq_optimization_data_queue) queue_destroy(resources->iq_optimization_data_queue);
}
