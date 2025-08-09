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
static bool validate_configuration(const AppConfig *config, const AppResources *resources);
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
static void application_progress_callback(unsigned long long current_read_frames, long long total_input_frames, unsigned long long total_output_frames, void* udata) {
    (void)total_output_frames;
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

    if (total_input_frames > 0) {
        double percentage = ((double)current_read_frames / (double)total_input_frames) * 100.0;
        if (percentage > 100.0) percentage = 100.0;
        fprintf(stderr, "\rProcessed %llu / %lld input frames (%.1f%%)...", current_read_frames, total_input_frames, percentage);
    } else {
        fprintf(stderr, "\rProcessed %llu input frames...", current_read_frames);
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
    int exit_status = EXIT_FAILURE;

    memset(&g_config, 0, sizeof(AppConfig));
    g_config.help_requested = false;
    initialize_resource_struct(&resources);
    reset_shutdown_flag();

    if (!presets_load_from_file(&g_config)) {
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    setup_signal_handlers(&resources);

    if (!parse_arguments(argc, argv, &g_config)) {
        print_usage(argv[0]);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    if (g_config.help_requested) {
        print_usage(argv[0]);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_SUCCESS;
    }

    if (!g_config.gain_provided) {
        g_config.gain = 1.0f;
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
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }

    resources.start_time = time(NULL);

#ifndef _WIN32
    pthread_t sig_thread_id;
    pthread_attr_t sig_thread_attr;
    if (pthread_attr_init(&sig_thread_attr) != 0) {
        log_fatal("Failed to initialize signal thread attributes.");
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    if (pthread_attr_setdetachstate(&sig_thread_attr, PTHREAD_CREATE_DETACHED) != 0) {
        log_fatal("Failed to set signal thread to detached state.");
        pthread_attr_destroy(&sig_thread_attr);
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    if (pthread_create(&sig_thread_id, &sig_thread_attr, signal_handler_thread, &resources) != 0) {
        log_fatal("Failed to create detached signal handler thread.");
        pthread_attr_destroy(&sig_thread_attr);
        cleanup_application(&g_config, &resources);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_FAILURE;
    }
    pthread_attr_destroy(&sig_thread_attr);
#endif

    static PipelineContext thread_args;
    thread_args.config = &g_config;
    thread_args.resources = &resources;

    log_info("Starting processing threads...");
    if (pthread_create(&resources.reader_thread_handle, NULL, reader_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.pre_processor_thread_handle, NULL, pre_processor_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.resampler_thread_handle, NULL, resampler_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.post_processor_thread_handle, NULL, post_processor_thread_func, &thread_args) != 0 ||
        pthread_create(&resources.writer_thread_handle, NULL, writer_thread_func, &thread_args) != 0 ||
        (g_config.iq_correction.enable && pthread_create(&resources.iq_optimization_thread_handle, NULL, iq_optimization_thread_func, &thread_args) != 0))
    {
        handle_fatal_thread_error("In Main: Failed to create one or more threads.", &resources);
    } else {
        // Wait for all threads to complete.
        pthread_join(resources.post_processor_thread_handle, NULL);
        pthread_join(resources.writer_thread_handle, NULL);
        pthread_join(resources.resampler_thread_handle, NULL);
        pthread_join(resources.pre_processor_thread_handle, NULL);
        if (g_config.iq_correction.enable) {
            pthread_join(resources.iq_optimization_thread_handle, NULL);
        }
        pthread_join(resources.reader_thread_handle, NULL);
    }

    // After all threads are joined, finalize the progress bar line ONLY on natural completion.
    // On Ctrl+C, the signal handler's log message already adds a newline.
    if (resources.end_of_stream_reached && !g_config.output_to_stdout) {
        pthread_mutex_lock(&g_console_mutex);
        if (isatty(fileno(stderr))) {
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        pthread_mutex_unlock(&g_console_mutex);
    }

    log_info("All processing threads have joined.");

    cleanup_application(&g_config, &resources);

    bool processing_ok = !resources.error_occurred;
    print_final_summary(&g_config, &resources, processing_ok);
    
    exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

    fflush(stderr);
    presets_free_loaded(&g_config);
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
    resources->end_of_stream_reached = false;
#if defined(WITH_SDRPLAY)
    resources->sdr_api_is_open = false;
#endif
#if defined(WITH_HACKRF)
    resources->hackrf_dev = NULL;
#endif
    g_config.iq_correction.enable = false;
    g_config.dc_block.enable = false;
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
        log_error("Application initialization failed. Cleaning up partially initialized resources.");
        cleanup_application(config, resources);
    } else if (!g_config.output_to_stdout) {
        bool is_sdr = resources->selected_input_ops->is_sdr_hardware();
        if (is_sdr) {
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
        log_error("Processing stopped after %llu input frames.", resources->total_frames_read);
        fprintf(stderr, "%-*s %s (possibly incomplete)\n", label_width, "Output File Size:", size_buf);
    } else if (resources->end_of_stream_reached) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Completed Successfully");
        fprintf(stderr, "%-*s %s\n", label_width, "Processing Duration:", duration_buf);
        fprintf(stderr, "%-*s %llu / %lld (100.0%%)\n", label_width, "Input Frames Processed:", resources->total_frames_read, (long long)resources->source_info.frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Generated:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Generated:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
    } else if (is_shutdown_requested()) {
        bool is_sdr_input = resources->selected_input_ops->is_sdr_hardware();
        if (is_sdr_input) {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Capture Stopped by User");
        } else {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Processing Cancelled by User");
        }
        const char* duration_label = is_sdr_input ? "Capture Duration:" : "Processing Duration:";
        fprintf(stderr, "%-*s %s\n", label_width, duration_label, duration_buf);
        if (is_sdr_input) {
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Frames Processed:", resources->total_frames_read);
        } else {
            double percentage = 0.0;
            if (resources->source_info.frames > 0) {
                percentage = ((double)resources->total_frames_read / (double)resources->source_info.frames) * 100.0;
            }
            fprintf(stderr, "%-*s %llu / %lld (%.1f%%)\n", label_width, "Input Frames Processed:", resources->total_frames_read, (long long)resources->source_info.frames, percentage);
        }
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Generated:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Generated:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
    }
    fprintf(stderr, "\n");
}
