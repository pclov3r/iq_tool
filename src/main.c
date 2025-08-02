// main.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#ifndef _WIN32
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <io.h> // For _isatty
#endif

#include "types.h"
#include "config.h"
#include "cli.h"
#include "setup.h"
#include "resample.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "signal_handler.h"
#include "log.h"
#include "input_manager.h"
#include "sample_convert.h"
#include "file_writer.h"
#include "presets_loader.h"

// --- Global Variable Definitions ---

// Define a single, global, recursive mutex for all console output.
// It is declared 'extern' in signal_handler.c so it can be used there.
pthread_mutex_t g_console_mutex;

// Define the global configuration struct.
// It is declared 'extern' in cli.c so it can be populated there.
AppConfig g_config;

// --- Forward Declarations for Static Helper Functions ---
static void initialize_resource_struct(AppResources *resources);
static bool validate_configuration(const AppConfig *config, const AppResources *resources);
static bool initialize_application(AppConfig *config, AppResources *resources);
static void cleanup_application(AppConfig *config, AppResources *resources);
static void print_final_summary(const AppConfig *config, const AppResources *resources, bool success);
static void format_duration(double total_seconds, char* buffer, size_t buffer_size);

// --- Logger Lock Function ---
// Provides the logging library with a way to use our global mutex.
static void console_lock_function(bool lock, void *udata) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)udata;
    if (lock) {
        pthread_mutex_lock(mutex);
    } else {
        pthread_mutex_unlock(mutex);
    }
}

// --- Progress Update Callback ---
// This function is passed to the processing pipeline to report progress.
static void application_progress_callback(unsigned long long current_read_frames, long long total_input_frames, unsigned long long total_output_frames, void* udata) {
    (void)total_output_frames; // This parameter is currently unused.
    pthread_mutex_t *console_mutex = (pthread_mutex_t *)udata;
    pthread_mutex_lock(console_mutex);

    // Only print progress if stderr is a terminal.
    #ifdef _WIN32
    if (_isatty(_fileno(stderr))) {
        fprintf(stderr, "\r                                                                               \r");
    }
    #else
    if (isatty(fileno(stderr))) {
        fprintf(stderr, "\r                                                                               \r");
    }
    #endif

    if (total_input_frames > 0) { // Finite source (e.g., WAV file)
        double percentage = ((double)current_read_frames / (double)total_input_frames) * 100.0;
        if (percentage > 100.0) percentage = 100.0;
        fprintf(stderr, "\rProcessed %llu / %lld input frames (%.1f%%)...",
                current_read_frames, total_input_frames, percentage);
    } else { // Infinite source (e.g., SDR stream)
        fprintf(stderr, "\rProcessed %llu input frames...", current_read_frames);
    }
    fflush(stderr);
    pthread_mutex_unlock(console_mutex);
}


// --- Main Application Entry Point ---
int main(int argc, char *argv[]) {
    // Initialize mutex attributes for a recursive mutex, allowing the same
    // thread to lock it multiple times without deadlocking.
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_console_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // Configure the logger to use our global mutex for thread-safe output.
    log_set_lock(console_lock_function, &g_console_mutex);
    log_set_level(LOG_INFO); // Set the default logging level.

    AppResources resources;
    int exit_status = EXIT_FAILURE;

    #ifndef _WIN32
    pthread_t sig_thread_id;
    #endif

    // --- CRUCIAL: Explicitly reset all global/static state ---
    // This ensures a clean state if main() were ever called multiple times
    // without a full process exit (e.g., in a testing harness).
    memset(&g_config, 0, sizeof(AppConfig));
    g_config.help_requested = false; // FIX: Initialize the new help flag.
    initialize_resource_struct(&resources);
    reset_shutdown_flag();

    // Load presets from file at the very beginning. This function handles
    // finding the file and logs info/warnings; it only returns false on a
    // fatal, unrecoverable error like a memory allocation failure.
    if (!presets_load_from_file(&g_config)) {
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    // Set up signal handlers (Ctrl+C) for graceful shutdown.
    setup_signal_handlers(&resources);

    // Parse command-line arguments.
    if (!parse_arguments(argc, argv, &g_config)) {
        // A 'false' return here indicates a genuine parsing error (e.g., unknown
        // option, missing value), not a help request.
        print_usage(argv[0]);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    // --- FIX: Check for the help flag after successful parsing ---
    // If --help was used, parse_arguments returns true and sets this flag.
    if (g_config.help_requested) {
        print_usage(argv[0]);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_SUCCESS; // Exit cleanly with success code.
    }

    // Get the correct set of input operations (WAV, SDRplay, etc.).
    resources.selected_input_ops = get_input_ops_by_name(g_config.input_type_str);
    if (!resources.selected_input_ops) {
        log_fatal("Input type '%s' is not supported or not enabled in this build.", g_config.input_type_str);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    // Validate the combination of options.
    if (!validate_configuration(&g_config, &resources)) {
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    // Initialize all application resources (buffers, DSP, threads, etc.).
    if (!initialize_application(&g_config, &resources)) {
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    resources.start_time = time(NULL);

    // On POSIX systems, spawn a dedicated thread to wait for signals.
    #ifndef _WIN32
    if (pthread_create(&sig_thread_id, NULL, signal_handler_thread, &resources) != 0) {
        log_fatal("Failed to create signal handler thread.");
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    #endif

    // Start the main processing pipeline threads.
    if (!run_processing_threads(&g_config, &resources)) {
        log_fatal("Failed to start processing threads.");
        #ifndef _WIN32
        pthread_cancel(sig_thread_id);
        pthread_join(sig_thread_id, NULL);
        #endif
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    // Wait for the processing pipeline to complete.
    pthread_join(resources.processor_thread, NULL);
    pthread_join(resources.writer_thread, NULL);

    // Clear the progress line from the console if it was being displayed.
    if (!g_config.output_to_stdout) {
        pthread_mutex_lock(&g_console_mutex);
        #ifdef _WIN32
        if (_isatty(_fileno(stderr))) { fprintf(stderr, "\r                                                                               \r"); }
        #else
        if (isatty(fileno(stderr))) { fprintf(stderr, "\r                                                                               \r"); }
        #endif
        pthread_mutex_unlock(&g_console_mutex);
    }

    log_info("Processor and writer threads have joined.");
    pthread_join(resources.reader_thread, NULL);
    log_info("Reader thread has joined.");

    // Clean up the signal handler thread on POSIX.
    #ifndef _WIN32
    pthread_cancel(sig_thread_id);
    pthread_join(sig_thread_id, NULL);
    #endif

    // Release all allocated resources.
    cleanup_application(&g_config, &resources);

    // Determine final exit status.
    bool processing_ok = !resources.error_occurred;
    print_final_summary(&g_config, &resources, processing_ok);
    exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

    fflush(stderr);

    // Free memory used by presets.
    presets_free_loaded(&g_config);

    // Destroy the global mutex.
    pthread_mutex_destroy(&g_console_mutex);
    return exit_status;
}


// --- Static Helper Function Implementations ---

static void initialize_resource_struct(AppResources *resources) {
    memset(resources, 0, sizeof(AppResources));
    resources->final_output_size_bytes = -1LL;
    resources->selected_input_ops = NULL;
    resources->progress_callback = NULL;
    resources->progress_callback_udata = NULL;
#if defined(WITH_SDRPLAY)
    resources->sdr_api_is_open = false;
#endif
#if defined(WITH_HACKRF)
    resources->hackrf_dev = NULL;
#endif
}

static bool validate_configuration(const AppConfig *config, const AppResources *resources) {
    if (resources->selected_input_ops->validate_options) {
        if (!resources->selected_input_ops->validate_options(config)) {
            return false;
        }
    }
    return true;
}

static bool initialize_application(AppConfig *config, AppResources *resources) {
    InputSourceContext ctx = { .config = config, .resources = resources };

    if (!resolve_file_paths(config)) return false;

    if (!resources->selected_input_ops->initialize(&ctx)) {
        return false;
    }

    if (config->native_8bit_path) {
        if (resources->input_pcm_format != PCM_FORMAT_S8 && resources->input_pcm_format != PCM_FORMAT_U8) {
            log_fatal("Invalid option: --no-8-to-16 was specified, but the input source is not 8-bit.");
            return false;
        }
    }

    setup_sample_converter(config, resources);

    float resample_ratio = 0.0f;
    if (!calculate_and_validate_resample_ratio(config, resources, &resample_ratio)) return false;
    if (!allocate_processing_buffers(config, resources, resample_ratio)) return false;
    if (!create_dsp_components(config, resources, resample_ratio)) return false;
    if (!create_threading_components(resources)) return false;

    resources->progress_callback = application_progress_callback;
    resources->progress_callback_udata = &g_console_mutex;

    if (!config->output_to_stdout) {
        print_configuration_summary(config, resources);
        if (!check_nyquist_warning(config, resources)) return false;
    }

    if (!prepare_output_stream(config, resources)) return false;

    return true;
}

static void cleanup_application(AppConfig *config, AppResources *resources) {
    if (!resources) return;

    InputSourceContext ctx = { .config = config, .resources = resources };

    if (resources->selected_input_ops && resources->selected_input_ops->cleanup) {
        resources->selected_input_ops->cleanup(&ctx);
    }

    if (resources->writer_ctx.ops.close) {
        resources->writer_ctx.ops.close(&resources->writer_ctx);
    }
    if (resources->writer_ctx.ops.get_total_bytes_written) {
        resources->final_output_size_bytes = resources->writer_ctx.ops.get_total_bytes_written(&resources->writer_ctx);
    }

    destroy_threading_components(resources);
    shift_destroy_nco(resources);
    if (resources->resampler) msresamp_crcf_destroy(resources->resampler);

    free(resources->work_item_pool);
    free(resources->raw_input_pool);
    free(resources->complex_scaled_pool);
    free(resources->complex_shifted_pool);
    free(resources->complex_resampled_pool);
    free(resources->output_pool);

    #ifdef _WIN32
    free_absolute_path_windows(&config->effective_input_filename_w, &config->effective_input_filename_utf8);
    free_absolute_path_windows(&config->effective_output_filename_w, &config->effective_output_filename_utf8);
    #endif
}

static void format_duration(double total_seconds, char* buffer, size_t buffer_size) {
    if (!isfinite(total_seconds) || total_seconds < 0) {
        snprintf(buffer, buffer_size, "N/A");
        return;
    }
    if (total_seconds > 0 && total_seconds < 1.0) {
        total_seconds = 1.0; // Report at least 1 second for very short runs
    }

    int hours = (int)(total_seconds / 3600);
    total_seconds -= hours * 3600;
    int minutes = (int)(total_seconds / 60);
    total_seconds -= minutes * 60;
    int seconds = (int)round(total_seconds);

    // Handle potential rounding carry-over
    if (seconds >= 60) { minutes++; seconds = 0; }
    if (minutes >= 60) { hours++; minutes = 0; }

    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
}

static void print_final_summary(const AppConfig *config, const AppResources *resources, bool success) {
    if (config->output_to_stdout) {
        return; // Don't print summary for stdout piping
    }

    const int label_width = 32;
    char size_buf[40];
    char duration_buf[40];
    format_file_size(resources->final_output_size_bytes, size_buf, sizeof(size_buf));
    double duration_secs = difftime(time(NULL), resources->start_time);
    format_duration(duration_secs, duration_buf, sizeof(duration_buf));
    unsigned long long total_output_samples = resources->total_output_frames * 2;

    fprintf(stderr, "\n--- Final Summary ---\n");

    if (success && !is_shutdown_requested()) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Completed Successfully");
        fprintf(stderr, "%-*s %s\n", label_width, "Processing Duration:", duration_buf);
        fprintf(stderr, "%-*s %lld / %lld (100.0%%)\n", label_width, "Input Frames Processed:",
                (long long)resources->source_info.frames, (long long)resources->source_info.frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Generated:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Generated:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);

    } else if (is_shutdown_requested()) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Cancelled by User");
        bool is_sdr_input = resources->selected_input_ops->is_sdr_hardware();
        const char* duration_label = is_sdr_input ? "Capture Duration:" : "Processing Duration:";
        fprintf(stderr, "%-*s %s\n", label_width, duration_label, duration_buf);

        if (is_sdr_input) {
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Frames Processed:", resources->total_frames_read);
        } else {
            double percentage = 0.0;
            if (resources->source_info.frames > 0) {
                percentage = ((double)resources->total_frames_read / (double)resources->source_info.frames) * 100.0;
            }
            fprintf(stderr, "%-*s %llu / %lld (%.1f%%)\n", label_width, "Input Frames Processed:",
                    resources->total_frames_read, (long long)resources->source_info.frames, percentage);
        }
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Generated:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Generated:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);

    } else { // Error occurred
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Stopped Due to Error");
        log_error("Processing stopped after %llu input frames.", resources->total_frames_read);
        fprintf(stderr, "%-*s %s (possibly incomplete)\n", label_width, "Output File Size:", size_buf);
    }
    fprintf(stderr, "\n");
}
