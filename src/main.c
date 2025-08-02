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
#include "presets_loader.h" // Include the new header for preset loading

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <io.h> // For _isatty
#endif

// Define a single, global, recursive mutex for all console output
pthread_mutex_t g_console_mutex;

// Make the global config accessible to other files that need it (like cli.c)
extern AppConfig g_config; // Already declared extern in cli.c, but good to have here too.
AppConfig g_config; // Actual definition in main.c

// Create a lock function for the logger to use our global mutex
static void console_lock_function(bool lock, void *udata) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)udata;
    if (lock) {
        pthread_mutex_lock(mutex);
    } else {
        pthread_mutex_unlock(mutex);
    }
}

// Progress update callback function
static void application_progress_callback(unsigned long long current_read_frames, long long total_input_frames, unsigned long long total_output_frames, void* udata) {
    (void)total_output_frames;
    pthread_mutex_t *console_mutex = (pthread_mutex_t *)udata;
    pthread_mutex_lock(console_mutex);
    #ifdef _WIN32
    if (_isatty(_fileno(stderr))) {
        fprintf(stderr, "\r                                                                               \r");
    }
    #else
    if (isatty(fileno(stderr))) {
        fprintf(stderr, "\r                                                                               \r");
    }
    #endif
    if (total_input_frames > 0) {
        double percentage = ((double)current_read_frames / (double)total_input_frames) * 100.0;
        if (percentage > 100.0) percentage = 100.0;
        fprintf(stderr, "\rProcessed %llu / %lld input frames (%.1f%%)...",
                current_read_frames, total_input_frames, percentage);
    } else {
        fprintf(stderr, "\rProcessed %llu input frames...", current_read_frames);
    }
    pthread_mutex_unlock(console_mutex);
}


// --- Forward Declarations for Static Helper Functions ---
static void initialize_resource_struct(AppResources *resources);
static bool validate_configuration(const AppConfig *config, const AppResources *resources);
static bool initialize_application(AppConfig *config, AppResources *resources);
static void cleanup_application(AppConfig *config, AppResources *resources);
static void print_final_summary(const AppConfig *config, const AppResources *resources, bool success);

int main(int argc, char *argv[]) {
    // Initialize mutex attributes for recursive mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_console_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    log_set_lock(console_lock_function, &g_console_mutex);
    log_set_level(LOG_INFO); // Default log level

    AppResources resources;
    int exit_status = EXIT_FAILURE;

    #ifndef _WIN32
    pthread_t sig_thread_id;
    #endif

    // --- CRUCIAL FIX: Explicitly reset global/static state for re-entry ---
    // This ensures a clean state if main() is called multiple times without a full process exit.
    memset(&g_config, 0, sizeof(AppConfig)); // Reset global config struct
    initialize_resource_struct(&resources); // Reset global resources struct
    reset_shutdown_flag(); // Reset the signal handler's shutdown flag
    // -----------------------------------------------------------------------

    // +++ STEP 1: LOAD PRESETS FROM FILE AT THE VERY BEGINNING +++
    // The presets_load_from_file function now handles searching for the file.
    // It returns false only on fatal system errors (e.g., malloc failure),
    // not for conflicts or file not found (which are logged as warnings/info).
    if (!presets_load_from_file(&g_config)) {
        // A false return from the loader is a fatal, unrecoverable error (e.g., malloc failed)
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    setup_signal_handlers(&resources);

    if (!parse_arguments(argc, argv, &g_config)) {
        print_usage(argv[0]);
        presets_free_loaded(&g_config); // Free presets before exiting
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    resources.selected_input_ops = get_input_ops_by_name(g_config.input_type_str);
    if (!resources.selected_input_ops) {
        log_fatal("Input type '%s' is not supported or not enabled in this build.", g_config.input_type_str);
        presets_free_loaded(&g_config); // Free presets before exiting
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    if (!validate_configuration(&g_config, &resources)) {
        presets_free_loaded(&g_config); // Free presets before exiting
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    if (!initialize_application(&g_config, &resources)) {
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config); // Free presets before exiting
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    resources.start_time = time(NULL);

    #ifndef _WIN32
    if (pthread_create(&sig_thread_id, NULL, signal_handler_thread, &resources) != 0) {
        log_fatal("Failed to create signal handler thread.");
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    #endif

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

    pthread_join(resources.processor_thread, NULL);
    pthread_join(resources.writer_thread, NULL);

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

    #ifndef _WIN32
    pthread_cancel(sig_thread_id);
    pthread_join(sig_thread_id, NULL);
    #endif

    cleanup_application(&g_config, &resources);

    bool processing_ok = !resources.error_occurred;
    print_final_summary(&g_config, &resources, processing_ok);
    exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

    fflush(stderr);
    
    // +++ STEP 2: FREE THE LOADED PRESETS AT THE VERY END +++
    presets_free_loaded(&g_config);

    pthread_mutex_destroy(&g_console_mutex);
    return exit_status;
}

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

    if (resources->selected_input_ops) {
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
        total_seconds = 1.0;
    }

    int hours = (int)(total_seconds / 3600);
    total_seconds -= hours * 3600;
    int minutes = (int)(total_seconds / 60);
    total_seconds -= minutes * 60;
    int seconds = (int)round(total_seconds);

    if (seconds >= 60) { minutes += seconds / 60; seconds %= 60; }
    if (minutes >= 60) { hours += minutes / 60; minutes %= 60; }
    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
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
    unsigned long long total_output_samples = resources->total_output_frames * 2;

    if (success && !is_shutdown_requested()) {
        fprintf(stderr, "\n");
        fprintf(stderr, "%-*s %s\n", label_width, "Processing Duration:", duration_buf);
        fprintf(stderr, "%-*s %lld / %lld (100.0%%)\n", label_width, "Input Frames Processed:",
                (long long)resources->source_info.frames, (long long)resources->source_info.frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Generated:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Generated:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
        fprintf(stderr, "\nResample operation completed successfully.\n");

    } else if (is_shutdown_requested()) {
        fprintf(stderr, "\n");
        
        bool is_sdr_input = resources->selected_input_ops->is_sdr_hardware();
        
        if (is_sdr_input) {
            fprintf(stderr, "%-*s %s\n", label_width, "Capture Duration:", duration_buf);
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Frames Processed:", resources->total_frames_read);
            fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Generated:", resources->total_output_frames);
            fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Generated:", total_output_samples);
            fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
        } else {
            double percentage = 0.0;
            if (resources->source_info.frames > 0) {
                percentage = ((double)resources->total_frames_read / (double)resources->source_info.frames) * 100.0;
            }
            fprintf(stderr, "%-*s %s\n", label_width, "Processing Duration:", duration_buf);
            fprintf(stderr, "%-*s %llu / %lld (%.1f%%)\n", label_width, "Input Frames Processed:",
                    resources->total_frames_read, (long long)resources->source_info.frames, percentage);
            fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Generated:", resources->total_output_frames);
            fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Generated:", total_output_samples);
            fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
            fprintf(stderr, "\nResample operation cancelled.\n");
        }

    } else {
        log_error("Processing stopped due to an error after %llu input frames.", resources->total_frames_read);
        fprintf(stderr, "%-*s %s (possibly incomplete)\n", label_width, "Output File Size:", size_buf);
    }
}
