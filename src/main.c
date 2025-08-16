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
// NOTE: initialize_application and cleanup_application are now public (declared in setup.h)
static void print_final_summary(const AppConfig *config, const AppResources *resources, bool success);
static void console_lock_function(bool lock, void *udata);
static void application_progress_callback(unsigned long long current_output_frames, long long total_output_frames, unsigned long long unused_arg, void* udata);


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
    
    memset(&g_config, 0, sizeof(AppConfig));
    input_manager_apply_defaults(&g_config);
    g_config.gain = 1.0f;

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

    if (argc <= 1) {
        print_usage(argv[0]);
        presets_free_loaded(&g_config);
        pthread_mutex_destroy(&g_console_mutex);
        return EXIT_SUCCESS;
    }

    if (!parse_arguments(argc, argv, &g_config)) {
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

    resources.progress_callback = application_progress_callback;
    resources.progress_callback_udata = &g_console_mutex;

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

    pthread_mutex_lock(&g_console_mutex);

    if ((resources.end_of_stream_reached || is_shutdown_requested()) && !g_config.output_to_stdout) {
        #ifdef _WIN32
        if (_isatty(_fileno(stderr))) {
        #else
        if (isatty(fileno(stderr))) {
        #endif
            fprintf(stderr, "\r                                                                               \r");
            fflush(stderr);
        }
    }

    log_debug("All processing threads have joined.");

    cleanup_application(&g_config, &resources);

    bool processing_ok = !resources.error_occurred;
    print_final_summary(&g_config, &resources, processing_ok);
    
    pthread_mutex_unlock(&g_console_mutex);

    int exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

    presets_free_loaded(&g_config);
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

static void console_lock_function(bool lock, void *udata) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)udata;
    if (lock) {
        pthread_mutex_lock(mutex);
    } else {
        pthread_mutex_unlock(mutex);
    }
}

static void application_progress_callback(unsigned long long current_output_frames, long long total_output_frames, unsigned long long unused_arg, void* udata) {
    (void)unused_arg;
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
        if (percentage > 100.0) percentage = 100.0;
        fprintf(stderr, "\rWriting output frames %llu / %lld (%.1f%% Est.)...", current_output_frames, total_output_frames, percentage);
    } else {
        fprintf(stderr, "\rWritten %llu output frames...", current_output_frames);
    }
    fflush(stderr);
    pthread_mutex_unlock(console_mutex);
}
