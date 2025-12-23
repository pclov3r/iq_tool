/*
 * This file is part of iq_tool.
 *
 * Copyright (C) 2025 iq_tool
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

#ifdef _WIN32
#include <windows.h>
#endif

#include "constants.h"
#include "app_context.h"
#include "signal_handler.h"
#include "log.h"
#include "module.h"
#include "pipeline_context.h"
#include "cli.h"
#include "initialization.h"
#include "utils.h"
#include "module_registry.h"
#include "presets_loader.h"
#include "platform.h"
#include "mem_arena.h"
#include "pipeline.h"
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


// --- Forward Declarations for Static Helper Functions ---
static void initialize_resource_struct(AppConfig *config, AppContext* app);
static bool validate_configuration(AppConfig *config, const AppContext* app);
static void print_final_summary(const AppConfig *config, const AppContext* app, bool success);
static void console_lock_function(bool lock, void *udata);
static void application_progress_callback(unsigned long long current_output_frames, long long total_output_frames, unsigned long long current_bytes_written, void* udata);
static const char* find_input_type_arg(int argc, char *argv[]);


// --- Main Application Entry Point ---
int main(int argc, char *argv[]) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    int exit_status = EXIT_FAILURE;
    AppContext app;
    bool resources_initialized = false;
    bool arena_initialized = false;

    AppConfig config;
    memset(&config, 0, sizeof(AppConfig));

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

    memset(&config, 0, sizeof(AppConfig));

    initialize_resource_struct(&config, &app);
    reset_shutdown_flag();
    setup_signal_handlers(&app);

    if (!mem_arena_init(&app.pipeline.setup_arena, MEM_ARENA_SIZE_BYTES)) {
        goto cleanup;
    }
    arena_initialized = true;

    // Phase 1: Pre-scan arguments to find the input type.
    const char* input_type = find_input_type_arg(argc, argv);
    if (input_type) {
        config.input.type_name = (char*)input_type;
        int num_modules = 0;
        const Module* modules = module_get_all(&num_modules, &app.pipeline.setup_arena);
        for (int i = 0; i < num_modules; ++i) {
            if (strcasecmp(input_type, modules[i].name) == 0) {
                if (modules[i].set_default_config) {
                    modules[i].set_default_config(&config);
                }
                break;
            }
        }
    }

    config.dsp.input_gain = 1.0f;
    config.dsp.output_gain = 1.0f;

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
    if (pthread_create(&sig_thread_id, &sig_thread_attr, signal_handler_thread, &app) != 0) {
        log_fatal("Failed to create detached signal handler thread.");
        pthread_attr_destroy(&sig_thread_attr);
        goto cleanup;
    }
    pthread_attr_destroy(&sig_thread_attr);
#endif

    if (!presets_load_from_file(&config, &app.pipeline.setup_arena)) {
        goto cleanup;
    }

    if (argc <= 1) {
        cli_print_usage(argv[0], &config, &app.pipeline.setup_arena);
        exit_status = EXIT_SUCCESS;
        goto cleanup;
    }

    // Phase 2: Call the main parser.
    if (!cli_parse(argc, argv, &config, &app.pipeline.setup_arena)) {
        goto cleanup;
    }

    const Module* _mod = module_get(config.input.type_name, MODULE_TYPE_INPUT, &app.pipeline.setup_arena); app.module.input_api = (_mod) ? (InputModuleInterface*)_mod->api : NULL;
    if (!app.module.input_api) {
        log_fatal("Input type '%s' is not supported or not enabled in this build.", config.input.type_name);
        goto cleanup;
    }

    if (!validate_configuration(&config, &app)) {
        goto cleanup;
    }

    if (!initialize_application(&config, &app)) {
        goto cleanup;
    }
    resources_initialized = true;

    app.stats.progress_callback = application_progress_callback;
    app.stats.progress_callback_udata = &g_console_mutex;

    app.stats.start_time = time(NULL);


    // The entire concurrent operation is now encapsulated in this single call.
    PipelineContext pipeline_context = { .config = &config, .app = &app };
    if (!pipeline_run(&pipeline_context)) {
        // pipeline_run handles its own internal cleanup. If it fails, we
        // just need to proceed to the main application cleanup.
        log_error("Pipeline execution failed.");
    }


    bool processing_ok = !app.stats.error_occurred;
    exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    pthread_mutex_lock(&g_console_mutex);

    bool final_ok = !app.stats.error_occurred;

    cleanup_application(&config, &app);

    if (resources_initialized) {
        print_final_summary(&config, &app, final_ok);
    }

    pthread_mutex_unlock(&g_console_mutex);

    if (arena_initialized) {
        mem_arena_destroy(&app.pipeline.setup_arena);
    }

    pthread_mutex_destroy(&g_console_mutex);

    return exit_status;
}


// --- Static Helper Function Definitions ---

/**
 * This function manually scans the command-line arguments to find the value
 * of the `--input` or `-i` option *before* the main argparse library is invoked.
 */
static const char* find_input_type_arg(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) {
            if (i + 1 < argc) {
                return argv[i + 1];
            }
        }
    }
    return NULL;
}

static void initialize_resource_struct(AppConfig *config, AppContext* app) {
    memset(app, 0, sizeof(AppContext));
    config->dsp.iq_correction.enable = false;
    config->dsp.dc_block.enable = false;
}

static bool validate_configuration(AppConfig *config, const AppContext* app) {
    (void)config;
    (void)app;
    return true;
}

static void print_final_summary(const AppConfig *config, const AppContext* app, bool success) {
    (void)config;

    // If the output target is not a file (e.g., stdout), don't print a summary.
    if (!app->module.pacing_is_required) {
        return;
    }

    const int label_width = 32;
    char size_buf[40];
    char duration_buf[40];

    utils_format_size(app->stats.final_output_size_bytes, size_buf, sizeof(size_buf));
    double duration_secs = difftime(time(NULL), app->stats.start_time);
    utils_format_duration(duration_secs, duration_buf, sizeof(duration_buf));

    unsigned long long total_input_samples = app->stats.total_frames_read * 2;
    unsigned long long total_output_samples = app->stats.total_output_frames * 2;

    double avg_write_speed_mbps = 0.0;
    if (duration_secs > 0.001) {
        avg_write_speed_mbps = (double)app->stats.final_output_size_bytes / (1024.0 * 1024.0) / duration_secs;
    }

    fprintf(stderr, "\n--- Final Summary ---\n");
    if (!success) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Stopped Due to Error");
        if (app->stats.total_frames_read > 0) {
            log_error("Processing stopped after %llu input frames.", app->stats.total_frames_read);
        }
        fprintf(stderr, "%-*s %s (possibly incomplete)\n", label_width, "Output File Size:", size_buf);
    } else if (app->stats.end_of_stream_reached) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Completed Successfully");
        fprintf(stderr, "%-*s %s\n", label_width, "Processing Duration:", duration_buf);
        fprintf(stderr, "%-*s %llu / %lld (100.0%%)\n", label_width, "Input Frames Read:", app->stats.total_frames_read, (long long)app->module.source_info.frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", app->stats.total_output_frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Written:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
        fprintf(stderr, "%-*s %.2f MB/s\n", label_width, "Average Write Speed:", avg_write_speed_mbps);
    } else if (is_shutdown_requested()) {
        bool source_has_known_length = app->module.input_api->has_known_length();
        if (!source_has_known_length) {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Capture Stopped by User");
        } else {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Processing Cancelled by User");
        }
        const char* duration_label = !source_has_known_length ? "Capture Duration:" : "Processing Duration:";
        fprintf(stderr, "%-*s %s\n", label_width, duration_label, duration_buf);
        if (!source_has_known_length) {
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Frames Read:", app->stats.total_frames_read);
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        } else {
            double percentage = 0.0;
            if (app->module.source_info.frames > 0) {
                percentage = ((double)app->stats.total_frames_read / (double)app->module.source_info.frames) * 100.0;
            }
            fprintf(stderr, "%-*s %llu / %lld (%.1f%%)\n", label_width, "Input Frames Read:", app->stats.total_frames_read, (long long)app->module.source_info.frames, percentage);
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        }
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", app->stats.total_output_frames);
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

    double current_time = utils_get_time();

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
