/*
 * This file is part of iq_resample_tool.
 *
 * Copyright (C) 2025 iq_resample_tool
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * This tool has undergone extensive, long-duration stability testing
 * using live, FM HD Radio signals. Special thanks to the
 * strong signal strength and highly repetitive playlist of KDON 102.5.
 * If the pipeline can survive that, it can survive anything.
 * It is, for all intents and purposes, Kendrick Lamar Certified.
 *
 * It should also be noted that this codebase is a two-time survivor of a
 * catastrophic 'rm -rf *' event in the wrong directory. Its continued
 * existence is a testament to the importance of git, off-site backups, and
 * the 'make clean' command.
 */

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
#include <signal.h>
#else
#include <sys/stat.h>
#include <io.h>
#endif

#include "types.h"
#include "config.h"
#include "cli.h"
#include "setup.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "signal_handler.h"
#include "log.h"
#include "input_manager.h"
#include "sample_convert.h"
#include "file_writer.h"
#include "presets_loader.h"
#include "platform.h"
#include "iq_correct.h"
#include "dc_block.h"
#include "io_threads.h"
#include "processing_threads.h"


// --- Global Variable Definitions ---
pthread_mutex_t g_console_mutex;
AppConfig g_config;


// --- Forward Declarations for Static Helper Functions ---
static void initialize_resource_struct(AppResources *resources);
static bool validate_configuration(AppConfig *config, const AppResources *resources);
static bool initialize_application(AppConfig *config, AppResources *resources);
static void cleanup_application(AppConfig *config, AppResources *resources);
static void print_final_summary(const AppConfig *config, const AppResources *resources, bool success);


// --- Logger Lock Function ---
static void console_lock_function(bool lock, void *udata) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)udata;
    if (lock) {
        pthread_mutex_lock(mutex);
    } else {
        pthread_mutex_unlock(mutex);
    }
}

// --- Progress Update Callback ---
static void application_progress_callback(unsigned long long current_output_frames, long long total_output_frames, unsigned long long unused_arg, void* udata) {
    (void)unused_arg; // This argument is no longer used but kept for signature compatibility
    pthread_mutex_t *console_mutex = (pthread_mutex_t *)udata;
    pthread_mutex_lock(console_mutex);

#ifdef _WIN32
    if (_isatty(_fileno(stderr))) {
        fprintf(stderr, "\r \r");
    }
#else
    if (isatty(fileno(stderr))) {
        fprintf(stderr, "\r \r");
    }
#endif

    if (total_output_frames > 0) {
        double percentage = ((double)current_output_frames / (double)total_output_frames) * 100.0;
        if (percentage > 100.0) percentage = 100.0; // Clamp to 100%
        fprintf(stderr, "\rWriting output frames %llu / %lld (%.1f%% Est.)...", current_output_frames, total_output_frames, percentage);
    } else {
        // This handles live streams where the total is unknown
        fprintf(stderr, "\rWritten %llu output frames...", current_output_frames);
    }
    fflush(stderr);
    pthread_mutex_unlock(console_mutex);
}


// --- Main Application Entry Point ---
int main(int argc, char *argv[]) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_console_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    log_set_lock(console_lock_function, &g_console_mutex);
    log_set_level(LOG_INFO);

    AppResources resources;
    
    // --- Initialize g_config with defaults ---
    memset(&g_config, 0, sizeof(AppConfig));
    input_manager_apply_defaults(&g_config); // Set module-specific defaults
    g_config.gain = 1.0f;                    // Set global defaults

    initialize_resource_struct(&resources);
    reset_shutdown_flag();
    setup_signal_handlers(&resources);

#ifndef _WIN32
    pthread_t sig_thread_id;
    pthread_attr_t sig_thread_attr;
    if (pthread_attr_init(&sig_thread_attr) != 0) {
        log_fatal("Failed to initialize signal thread attributes.");
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    if (pthread_attr_setdetachstate(&sig_thread_attr, PTHREAD_CREATE_DETACHED) != 0) {
        log_fatal("Failed to set signal thread to detached state.");
        pthread_attr_destroy(&sig_thread_attr);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    if (pthread_create(&sig_thread_id, &sig_thread_attr, signal_handler_thread, &resources) != 0) {
        log_fatal("Failed to create detached signal handler thread.");
        pthread_attr_destroy(&sig_thread_attr);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    pthread_attr_destroy(&sig_thread_attr);
#endif

    if (!presets_load_from_file(&g_config)) {
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    // If no arguments are provided, show the help screen and exit.
    if (argc <= 1) {
        print_usage(argv[0]);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_SUCCESS;
    }

    if (!parse_arguments(argc, argv, &g_config)) {
        // Error messages are printed inside parse_arguments/argparse
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    resources.selected_input_ops = get_input_ops_by_name(g_config.input_type_str);
    if (!resources.selected_input_ops) {
        log_fatal("Input type '%s' is not supported or not enabled in this build.", g_config.input_type_str);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    if (!validate_configuration(&g_config, &resources)) {
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    if (!initialize_application(&g_config, &resources)) {
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    resources.start_time = time(NULL);

    static PipelineContext thread_args;
    thread_args.config = &g_config;
    thread_args.resources = &resources;

    log_debug("Starting processing threads...");
    if (pthread_create(&resources.reader_thread_handle, NULL, reader_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.pre_processor_thread_handle, NULL, pre_processor_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.resampler_thread_handle, NULL, resampler_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.post_processor_thread_handle, NULL, post_processor_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.writer_thread_handle, NULL, writer_thread_func, &thread_args) != 0 ||
        (g_config.iq_correction.enable && pthread_create(&resources.iq_optimization_thread_handle, NULL, iq_optimization_thread_func, &thread_args) != 0))
    {
        handle_fatal_thread_error("In Main: Failed to create one or more threads.", &resources);
    }

    pthread_join(resources.post_processor_thread_handle, NULL);
    pthread_join(resources.writer_thread_handle, NULL);
    pthread_join(resources.resampler_thread_handle, NULL);
    pthread_join(resources.pre_processor_thread_handle, NULL);
    if (g_config.iq_correction.enable) {
        pthread_join(resources.iq_optimization_thread_handle, NULL);
    }
    pthread_join(resources.reader_thread_handle, NULL);

    // --- MODIFIED SECTION ---
    // Lock the console mutex to ensure the final output is atomic and clean.
    pthread_mutex_lock(&g_console_mutex);

    if (resources.end_of_stream_reached && !g_config.output_to_stdout) {
        if (isatty(fileno(stderr))) {
            fprintf(stderr, "\r                                                                               \r");
            fflush(stderr);
        }
    }

    log_debug("All processing threads have joined.");

    cleanup_application(&g_config, &resources);

    bool processing_ok = !resources.error_occurred;
    print_final_summary(&g_config, &resources, processing_ok);
    
    // Unlock the console after all final printing is complete.
    pthread_mutex_unlock(&g_console_mutex);
    // --- END MODIFIED SECTION ---

    int exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

    presets_free_loaded(&g_config);
    pthread_mutex_destroy(&g_console_mutex);

    return exit_status;
}


static void initialize_resource_struct(AppResources *resources) {
    memset(resources, 0, sizeof(AppResources));
    
    // All pointers and numeric types in the resources struct are now zero-initialized by memset.
    // This includes SDR device handles (e.g., resources->hackrf_dev) which are correctly set to NULL,
    // and flags (e.g., resources->sdr_api_is_open) which are correctly set to false.

    // Explicitly initialize any non-zero default values or global config flags here.
    g_config.iq_correction.enable = false;
    g_config.dc_block.enable = false;
}

static bool validate_configuration(AppConfig *config, const AppResources *resources) {
    if (resources->selected_input_ops->validate_options) {
        if (!resources->selected_input_ops->validate_options(config)) {
            return false;
        }
    }
    return true;
}

static bool initialize_application(AppConfig *config, AppResources *resources) {
    bool success = false;
    resources->config = config;
    InputSourceContext ctx = { .config = config, .resources = resources };
    float resample_ratio = 0.0f;

    if (!resolve_file_paths(config)) goto cleanup;
    if (!resources->selected_input_ops->initialize(&ctx)) goto cleanup;
    if (!calculate_and_validate_resample_ratio(config, resources, &resample_ratio)) goto cleanup;
    if (!allocate_processing_buffers(config, resources, resample_ratio)) goto cleanup;
    if (!create_dsp_components(config, resources, resample_ratio)) goto cleanup;
    if (!create_threading_components(resources)) goto cleanup;

    resources->progress_callback = application_progress_callback;
    resources->progress_callback_udata = &g_console_mutex;

    if (!config->output_to_stdout) {
        print_configuration_summary(config, resources);
        if (!check_nyquist_warning(config, resources)) {
            goto cleanup;
        }
    }

    if (!prepare_output_stream(config, resources)) goto cleanup;

    if (config->dc_block.enable) {
        if (!dc_block_init(config, resources)) goto cleanup;
    }

    if (config->iq_correction.enable) {
        if (!iq_correct_init(config, resources)) goto cleanup;
    }

    success = true;

cleanup:
    if (!success) {
        log_error("Application initialization failed.");
    } else if (!g_config.output_to_stdout) {
        bool source_has_known_length = resources->selected_input_ops->has_known_length();
        if (!source_has_known_length) {
            log_info("Starting SDR capture...");
        } else {
            log_info("Starting file processing...");
        }
    }
    return success;
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

    if (config->dc_block.enable) {
        dc_block_cleanup(resources);
    }
    if (config->iq_correction.enable) {
        iq_correct_cleanup(resources);
    }

    destroy_threading_components(resources);
    shift_destroy_ncos(resources);

    if (resources->resampler) {
        msresamp_crcf_destroy(resources->resampler);
        resources->resampler = NULL;
    }

    free(resources->sample_chunk_pool);
    free(resources->raw_input_data_pool);
    free(resources->complex_pre_resample_data_pool);
    free(resources->complex_post_resample_data_pool);
    free(resources->complex_resampled_data_pool);
    free(resources->final_output_data_pool);

#ifdef _WIN32
    free_absolute_path_windows(&config->effective_input_filename_w, &config->effective_input_filename_utf8);
    free_absolute_path_windows(&config->effective_output_filename_w, &config->effective_output_filename_utf8);
#endif
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
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Written:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
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
        } else {
            double percentage = 0.0;
            if (resources->source_info.frames > 0) {
                percentage = ((double)resources->total_frames_read / (double)resources->source_info.frames) * 100.0;
            }
            fprintf(stderr, "%-*s %llu / %lld (%.1f%%)\n", label_width, "Input Frames Read:", resources->total_frames_read, (long long)resources->source_info.frames, percentage);
        }
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Written:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
    }
}
