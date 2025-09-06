#ifdef _WIN32
#include <windows.h>
#endif

#include "io_threads.h"
#include "constants.h"
#include "app_context.h"
#include "signal_handler.h"
#include "log.h"
#include "input_source.h"
#include "queue.h"
#include "sdr_packet_serializer.h"
#include "pipeline_context.h"
#include "file_write_buffer.h"
#include "cli.h"
#include "setup.h"
#include "utils.h"
#include "input_manager.h"
#include "sample_convert.h"
#include "file_writer.h"
#include "presets_loader.h"
#include "platform.h"
#include "memory_arena.h"
#include "iq_correct.h"
#include "dc_block.h"
#include "processing_threads.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <errno.h>

#ifndef _WIN32
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#else
#include <sys/stat.h>
#include <io.h>
#endif


// --- Global Variable Definitions ---
pthread_mutex_t g_console_mutex;
AppConfig g_config;


// --- Forward Declarations for Static Helper Functions ---
static void initialize_resource_struct(AppResources *resources);
static bool validate_configuration(AppConfig *config, const AppResources *resources);
static void print_final_summary(const AppConfig *config, const AppResources *resources, bool success);
static void console_lock_function(bool lock, void *udata);
static void application_progress_callback(unsigned long long current_output_frames, long long total_output_frames, unsigned long long current_bytes_written, void* udata);

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


// --- Main Application Entry Point ---
int main(int argc, char *argv[]) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    int exit_status = EXIT_FAILURE;
    AppResources resources;
    bool resources_initialized = false;
    bool arena_initialized = false;

    int ret;

    pthread_mutexattr_t attr;
    if ((ret = pthread_mutexattr_init(&attr)) != 0) {
        fprintf(stderr, "FATAL: Failed to initialize mutex attributes: %s\n", strerror(ret));
        return EXIT_FAILURE;
    }
    if ((ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) != 0) {
        fprintf(stderr, "FATAL: Failed to set mutex type to recursive: %s\n", strerror(ret));
        pthread_mutexattr_destroy(&attr);
        return EXIT_FAILURE;
    }
    if ((ret = pthread_mutex_init(&g_console_mutex, &attr)) != 0) {
        fprintf(stderr, "FATAL: Failed to initialize console mutex: %s\n", strerror(ret));
        pthread_mutexattr_destroy(&attr);
        return EXIT_FAILURE;
    }
    pthread_mutexattr_destroy(&attr);

    log_set_lock(console_lock_function, &g_console_mutex);
    log_set_level(LOG_INFO);

    memset(&g_config, 0, sizeof(AppConfig));

    initialize_resource_struct(&resources);
    reset_shutdown_flag();
    setup_signal_handlers(&resources);

    if (!mem_arena_init(&resources.setup_arena, MEM_ARENA_SIZE_BYTES)) {
        goto cleanup;
    }
    arena_initialized = true;

    input_manager_apply_defaults(&g_config, &resources.setup_arena);
    g_config.gain = 1.0f;

#ifndef _WIN32
    pthread_t sig_thread_id;
    pthread_attr_t sig_thread_attr;
    if (pthread_attr_init(&sig_thread_attr) != 0) {
        log_fatal("Failed to initialize signal thread attributes.");
        goto cleanup;
    }
    if (pthread_attr_setdetachstate(&sig_thread_attr, PTHREAD_CREATE_DETACHED) != 0) {
        log_fatal("Failed to set signal thread to detached state.");
        pthread_attr_destroy(&sig_thread_attr);
        goto cleanup;
    }
    if (pthread_create(&sig_thread_id, &sig_thread_attr, signal_handler_thread, &resources) != 0) {
        log_fatal("Failed to create detached signal handler thread.");
        pthread_attr_destroy(&sig_thread_attr);
        goto cleanup;
    }
    pthread_attr_destroy(&sig_thread_attr);
#endif

    if (!presets_load_from_file(&g_config, &resources.setup_arena)) {
        goto cleanup;
    }

    if (argc <= 1) {
        print_usage(argv[0], &g_config, &resources.setup_arena);
        exit_status = EXIT_SUCCESS;
        goto cleanup;
    }

    if (!parse_arguments(argc, argv, &g_config, &resources.setup_arena)) {
        goto cleanup;
    }

    resources.selected_input_ops = get_input_ops_by_name(g_config.input_type_str, &resources.setup_arena);
    if (!resources.selected_input_ops) {
        log_fatal("Input type '%s' is not supported or not enabled in this build.", g_config.input_type_str);
        goto cleanup;
    }

    if (!validate_configuration(&g_config, &resources)) {
        goto cleanup;
    }

    if (!initialize_application(&g_config, &resources)) {
        goto cleanup;
    }
    resources_initialized = true;

    resources.progress_callback = application_progress_callback;
    resources.progress_callback_udata = &g_console_mutex;

    resources.start_time = time(NULL);

    static PipelineContext thread_args;
    thread_args.config = &g_config;
    thread_args.resources = &resources;

    // Define the list of threads to be started.
    typedef struct {
        const char* name;
        void* (*func)(void*);
        bool required;
    } ThreadStarter;

    ThreadStarter threads_to_start[] = {
        { "SDR Capture",   sdr_capture_thread_func,     (resources.pipeline_mode == PIPELINE_MODE_BUFFERED_SDR) },
        { "Reader",        reader_thread_func,          true },
        { "Pre-Processor", pre_processor_thread_func,   true },
        { "Resampler",     resampler_thread_func,       true },
        { "Post-Processor",post_processor_thread_func,  true },
        { "Writer",        writer_thread_func,          true },
        { "I/Q Optimizer", iq_optimization_thread_func, g_config.iq_correction.enable },
        { "SDR Watchdog",  watchdog_thread_func,        is_sdr_input(g_config.input_type_str, &resources.setup_arena) },
    };
    int num_thread_starters = sizeof(threads_to_start) / sizeof(threads_to_start[0]);

    log_debug("Starting processing threads...");

    // Initialize the heartbeat time before starting threads.
    pthread_mutex_lock(&resources.progress_mutex);
    resources.last_sdr_heartbeat_time = get_monotonic_time_sec();
    pthread_mutex_unlock(&resources.progress_mutex);

    for (int i = 0; i < num_thread_starters; ++i) {
        if (threads_to_start[i].required) {
            if (pthread_create(&resources.thread_handles[resources.num_threads_started], NULL, threads_to_start[i].func, &thread_args) != 0) {
                log_fatal("Failed to create %s thread: %s", threads_to_start[i].name, strerror(errno));
                // A thread failed to create. Signal all already-running threads to shut down.
                request_shutdown();
                goto thread_join;
            }
            // Only increment the counter on success.
            resources.num_threads_started++;
        }
    }

thread_join:
    // This block is now safe to enter even on a partial startup failure.
    if (resources.num_threads_started > 0) {
        log_debug("Waiting for processing threads to complete...");
        for (int i = 0; i < resources.num_threads_started; i++) {
            // Use a standard blocking join. The watchdog is the safety net against hangs.
            if (pthread_join(resources.thread_handles[i], NULL) != 0) {
                log_warn("Error joining thread %d.", i);
            }
        }
        log_debug("All processing threads have joined successfully.");
    }

    bool processing_ok = !resources.error_occurred;
    exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    pthread_mutex_lock(&g_console_mutex);

    bool final_ok = !resources.error_occurred;
    cleanup_application(&g_config, &resources);

    if (resources_initialized) {
        print_final_summary(&g_config, &resources, final_ok);
    }

    pthread_mutex_unlock(&g_console_mutex);

    if (arena_initialized) {
        mem_arena_destroy(&resources.setup_arena);
    }

    pthread_mutex_destroy(&g_console_mutex);

    return exit_status;
}


// --- Static Helper Function Definitions ---

static void initialize_resource_struct(AppResources *resources) {
    memset(resources, 0, sizeof(AppResources));
    g_config.iq_correction.enable = false;
    g_config.dc_block.enable = false;
}

static bool validate_configuration(AppConfig *config, const AppResources *resources) {
    (void)config;
    (void)resources;
    return true;
}

static void print_final_summary(const AppConfig *config, const AppResources *resources, bool success) {
    if (config->output_to_stdout) {
        return;
    }
    const int label_width = 32;
    char size_buf[40];
    char duration_buf[40];

    format_file_size(resources->final_output_size_bytes, size_buf, sizeof(size_buf));
    double duration_secs = difftime(time(NULL), resources->start_time);
    format_duration(duration_secs, duration_buf, sizeof(duration_buf));

    unsigned long long total_input_samples = resources->total_frames_read * 2;
    unsigned long long total_output_samples = resources->total_output_frames * 2;

    double avg_write_speed_mbps = 0.0;
    if (duration_secs > 0.001) {
        avg_write_speed_mbps = (double)resources->final_output_size_bytes / (1024.0 * 1024.0) / duration_secs;
    }

    fprintf(stderr, "\n--- Final Summary ---\n");
    if (!success) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Stopped Due to Error");
        if (resources->total_frames_read > 0) {
            log_error("Processing stopped after %llu input frames.", resources->total_frames_read);
        }
        fprintf(stderr, "%-*s %s (possibly incomplete)\n", label_width, "Output File Size:", size_buf);
    } else if (resources->end_of_stream_reached) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Completed Successfully");
        fprintf(stderr, "%-*s %s\n", label_width, "Processing Duration:", duration_buf);
        fprintf(stderr, "%-*s %llu / %lld (100.0%%)\n", label_width, "Input Frames Read:", resources->total_frames_read, (long long)resources->source_info.frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Written:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
        fprintf(stderr, "%-*s %.2f MB/s\n", label_width, "Average Write Speed:", avg_write_speed_mbps);
    } else if (is_shutdown_requested()) {
        bool source_has_known_length = resources->selected_input_ops->has_known_length();
        if (!source_has_known_length) {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Capture Stopped by User");
        } else {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Processing Cancelled by User");
        }
        const char* duration_label = !source_has_known_length ? "Capture Duration:" : "Processing Duration:";
        fprintf(stderr, "%-*s %s\n", label_width, duration_label, duration_buf);
        if (!source_has_known_length) {
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Frames Read:", resources->total_frames_read);
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        } else {
            double percentage = 0.0;
            if (resources->source_info.frames > 0) {
                percentage = ((double)resources->total_frames_read / (double)resources->source_info.frames) * 100.0;
            }
            fprintf(stderr, "%-*s %llu / %lld (%.1f%%)\n", label_width, "Input Frames Read:", resources->total_frames_read, (long long)resources->source_info.frames, percentage);
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        }
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Written:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
        fprintf(stderr, "%-*s %.2f MB/s\n", label_width, "Average Write Speed:", avg_write_speed_mbps);
    }
}

static void console_lock_function(bool lock, void *udata) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)udata;
    if (lock) {
        pthread_mutex_lock(mutex);
    } else {
        pthread_mutex_unlock(mutex);
    }
}

static void application_progress_callback(unsigned long long current_output_frames, long long total_output_frames, unsigned long long current_bytes_written, void* udata) {
    (void)udata;

    if (PROGRESS_UPDATE_INTERVAL_SECONDS == 0) {
        return;
    }

    static double last_progress_log_time = 0.0;
    static long long last_bytes_written = 0;

    double current_time = get_monotonic_time_sec();

    if (current_time - last_progress_log_time >= PROGRESS_UPDATE_INTERVAL_SECONDS) {
        double rate_mb_per_sec = 0.0;
        if (last_progress_log_time > 0.0) {
            long long bytes_delta = current_bytes_written - last_bytes_written;
            double time_delta = current_time - last_progress_log_time;
            if (time_delta > 0) {
                rate_mb_per_sec = (double)bytes_delta / (1024.0 * 1024.0) / time_delta;
            }
        }

        bool is_first_update = (last_progress_log_time == 0.0);

        if (total_output_frames > 0) {
            double percentage = ((double)current_output_frames / (double)total_output_frames) * 100.0;
            if (percentage > 100.0) percentage = 100.0;
            if (is_first_update) {
                log_info("Writing: %llu / ~%lld frames (%.1f%%)",
                         current_output_frames, total_output_frames, percentage);
            } else {
                log_info("Writing: %llu / ~%lld frames (%.1f%%) %.2f MB/s",
                         current_output_frames, total_output_frames, percentage, rate_mb_per_sec);
            }
        } else {
            if (is_first_update) {
                log_info("Written %llu frames", current_output_frames);
            } else {
                log_info("Written %llu frames %.2f MB/s",
                         current_output_frames, rate_mb_per_sec);
            }
        }

        last_progress_log_time = current_time;
        last_bytes_written = current_bytes_written;
    }
}
