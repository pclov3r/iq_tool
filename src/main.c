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
#include "utils.h"
#include "module_registry.h"
#include "presets_loader.h"
#include "platform.h"
#include "mem_arena.h"
#include "sample_format_table.h"
#include "pipeline.h"
#include "agc.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <locale.h>

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
static void print_configuration_summary(const AppConfig *config, const AppContext* app);
static void print_final_summary(const AppConfig *config, const AppContext* app, bool success);
static void console_lock_function(bool lock, void *udata);
static void application_progress_callback(unsigned long long current_output_frames, long long total_output_frames, unsigned long long current_bytes_written, void* udata);
static bool init_input_source(AppConfig *config, AppContext* app);
static bool init_output_module(AppConfig *config, AppContext* app);
static void close_input_source(AppConfig *config, AppContext* app);
static void close_output_module(AppConfig *config, AppContext* app);

// --- Main Application Entry Point ---

int main(int argc, char *argv[]) {
    setlocale(LC_NUMERIC, "C");
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

    initialize_resource_struct(&config, &app);
    reset_shutdown_flag();
    setup_signal_handlers(&app);

    if (!mem_arena_init(&app.pipeline.setup_arena, MEM_ARENA_SIZE_BYTES)) {
        goto cleanup;
    }
    arena_initialized = true;

    if (!presets_load_from_file(&config, &app.pipeline.setup_arena)) {
        goto cleanup;
    }

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

    if (argc <= 1) {
        cli_print_usage(argv[0], &config, &app.pipeline.setup_arena);
        exit_status = EXIT_SUCCESS;
        goto cleanup;
    }

    if (!cli_parse(argc, argv, &config, &app.pipeline.setup_arena)) {
        goto cleanup;
    }

    if (!validate_configuration(&config, &app)) {
        goto cleanup;
    }

    // Apply frequency offset logic before initializing hardware
    if (config.sdr_general.frequency_offset_hz != 0.0f && config.sdr_general.rf_freq_provided) {
        double user_target_hz = config.sdr_general.rf_freq_hz;
        config.sdr_general.rf_freq_hz += config.sdr_general.frequency_offset_hz;

        log_info("Applying Frequency Offset: Target %.15g Hz + Offset %+.15g Hz = Tuning to %.15g Hz",
                 user_target_hz,
                 config.sdr_general.frequency_offset_hz,
                 config.sdr_general.rf_freq_hz);

        if (config.sdr_general.rf_freq_hz <= 0.0) {
            log_error("Resulting hardware frequency is %.15g Hz, which is not valid. Aborting.", config.sdr_general.rf_freq_hz);
            goto cleanup;
        }
    }

    if (!init_input_source(&config, &app)) {
        goto cleanup;
    }
    // Perform pre-stream calibration if needed (requires open source)
    if (app.module.input_api->pre_stream_iq_correction) {
        ModuleContext ctx = { .config = &config, .app = &app };
        if (!app.module.input_api->pre_stream_iq_correction(&ctx)) goto cleanup;
    }
    PipelineContext pipeline_context = { .config = &config, .app = &app };
    if (!pipeline_setup_buffers(&pipeline_context)) {
        goto cleanup;
    }

    if (!init_output_module(&config, &app)) {
        goto cleanup;
    }
    resources_initialized = true;

    print_configuration_summary(&config, &app);
    fprintf(stderr, "\n");

    if (app.pipeline_mode == PIPELINE_MODE_BUFFERED_INPUT) {
        log_info("Starting live source capture...");
    } else {
        log_info("Starting file processing...");
    }

    app.stats.progress_callback = application_progress_callback;
    app.stats.progress_callback_udata = &g_console_mutex;

    app.stats.start_time = time(NULL);

    if (!pipeline_run(&pipeline_context)) {
        log_fatal("Pipeline execution failed.");
    }

    bool processing_ok = !app.stats.error_occurred;
    exit_status = (processing_ok || is_shutdown_requested()) ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    pthread_mutex_lock(&g_console_mutex);

    bool final_ok = !app.stats.error_occurred;

    close_input_source(&config, &app);
    close_output_module(&config, &app);

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

// --- Input/Output Lifecycle Management ---

static bool init_input_source(AppConfig *config, AppContext* app) {
    app->config = config;
    app->dsp.config = config;
    ModuleContext ctx = { .config = config, .app = app };

    const Module* selected_input_module = module_get(config->input.type_name, MODULE_TYPE_INPUT, &app->pipeline.setup_arena);
    if (!selected_input_module) {
        log_error("Input type '%s' is not supported or enabled in this build.", config->input.type_name);
        return false;
    }
    app->module.input_api = (InputModuleInterface*)selected_input_module->api;
    app->pipeline_mode = selected_input_module->pipeline_mode;


    log_info("Initializing the '%s' input module...", config->input.type_name);
    return app->module.input_api->initialize(&ctx);
}

static bool init_output_module(AppConfig *config, AppContext* app) {
    ModuleContext ctx = { .config = config, .app = app };

    const Module* selected_output_module = module_get(config->output.module_name, MODULE_TYPE_OUTPUT, &app->pipeline.setup_arena);
    if (!selected_output_module) {
        log_fatal("Internal error: Could not retrieve selected output module.");
        return false;
    }
    app->module.output_api = (OutputModuleInterface*)selected_output_module->api;

    if (config->dsp.raw_passthrough && app->module.input_format != config->output.sample_format) {
        log_error("Option --raw-passthrough requires input and output formats to be identical.");
        return false;
    }

    log_info("Initializing the '%s' output module...", config->output.module_name);
    return app->module.output_api->initialize(&ctx);
}

static void close_input_source(AppConfig *config, AppContext* app) {
    if (!app || !app->module.input_api) return;
    ModuleContext ctx = { .config = config, .app = app };
    if (app->module.input_api->cleanup) {
        app->module.input_api->cleanup(&ctx);
    }
}

static void close_output_module(AppConfig *config, AppContext* app) {
    if (!app || !app->module.output_api) return;
    ModuleContext ctx = { .config = config, .app = app };
    if (app->module.output_api->cleanup) {
        app->module.output_api->cleanup(&ctx);
    }
}


// --- Static Helper Function Definitions ---

static void initialize_resource_struct(AppConfig *config, AppContext* app) {
    memset(app, 0, sizeof(AppContext));

    // Set global DSP defaults
    config->dsp.input_gain = 1.0f;
    config->dsp.output_gain = 1.0f;

    config->dsp.iq_correction.enable = false;
    config->dsp.dc_block.enable = false;
}

static bool validate_configuration(AppConfig *config, const AppContext* app) {
    (void)config;
    (void)app;
    return true;
}

static void print_configuration_summary(const AppConfig *config, const AppContext* app) {
    if (!config || !app || !app->module.input_api) return;

    InputSummaryInfo summary_info;
    memset(&summary_info, 0, sizeof(InputSummaryInfo));
    const ModuleContext ctx = { .config = config, .app = (AppContext*)app };
    app->module.input_api->get_summary_info(&ctx, &summary_info);

    int max_label_len = 0;
    if (summary_info.count > 0) {
        for (int i = 0; i < summary_info.count; i++) {
            int len = (int)strlen(summary_info.items[i].label);
            if (len > max_label_len) {
                max_label_len = len;
            }
        }
    }

    if (config->sdr_general.rf_freq_provided) {
        const char* offset_labels[] = { "Actual Frequency", "Frequency Offset", "Tuned Frequency", "RF Frequency" };
        for (int i = 0; i < 4; i++) {
            int len = (int)strlen(offset_labels[i]);
            if (len > max_label_len) max_label_len = len;
        }
    }

    const char* base_output_labels[] = {
        "Output Type", "Sample Type", "Output Sample Rate", "Input Gain", "Output Gain", "Frequency Shift",
        "Resampling", "Output Target", "FIR Filter", "FFT Filter", "Output AGC"
    };
    for (size_t i = 0; i < sizeof(base_output_labels) / sizeof(base_output_labels[0]); i++) {
        int len = (int)strlen(base_output_labels[i]);
        if (len > max_label_len) {
            max_label_len = len;
        }
    }

    fprintf(stderr, "\n--- Input Details ---\n");
    if (summary_info.count > 0) {
        for (int i = 0; i < summary_info.count; i++) {
            fprintf(stderr, " %-*s : %s\n", max_label_len, summary_info.items[i].label, summary_info.items[i].value);
        }
    }

    if (config->sdr_general.rf_freq_provided) {
        if (fabs(config->sdr_general.frequency_offset_hz) > 1e-9) {
            double user_target_hz = config->sdr_general.rf_freq_hz - config->sdr_general.frequency_offset_hz;
            fprintf(stderr, " %-*s : %.15g Hz\n", max_label_len, "Actual Frequency", user_target_hz);
            fprintf(stderr, " %-*s : %+.0f Hz\n", max_label_len, "Frequency Offset", config->sdr_general.frequency_offset_hz);
            fprintf(stderr, " %-*s : %.15g Hz\n", max_label_len, "Tuned Frequency", config->sdr_general.rf_freq_hz);
        } else {
            fprintf(stderr, " %-*s : %.15g Hz\n", max_label_len, "RF Frequency", config->sdr_general.rf_freq_hz);
        }
    }

    fprintf(stderr, " %-*s : %s\n", max_label_len, "I/Q Correction", config->dsp.iq_correction.enable ? "Enabled" : "Disabled");
    fprintf(stderr, " %-*s : %s\n", max_label_len, "DC Block", config->dsp.dc_block.enable ? "Enabled" : "Disabled");


    fprintf(stderr, "--- Output Details ---\n");
    if (app->module.output_api && app->module.output_api->get_summary_info) {
        OutputSummaryInfo output_summary;
        memset(&output_summary, 0, sizeof(output_summary));
        app->module.output_api->get_summary_info(&ctx, &output_summary);
        for (int i = 0; i < output_summary.count; i++) {
            fprintf(stderr, " %-*s : %s\n", max_label_len, output_summary.items[i].label, output_summary.items[i].value);
        }
    }

    const char* sample_type_str = get_format_info_by_enum(config->output.sample_format) ? get_format_info_by_enum(config->output.sample_format)->description_str : "Unknown";
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Sample Type", sample_type_str);

    fprintf(stderr, " %-*s : %.15g Hz\n", max_label_len, "Output Sample Rate", config->output_sample_rate.rate_hz);

    fprintf(stderr, " %-*s : %.5f\n", max_label_len, "Input Gain", config->dsp.input_gain);

    if (config->dsp.output_gain != 1.0f) {
        fprintf(stderr, " %-*s : %.5f\n", max_label_len, "Output Gain", config->dsp.output_gain);
    }

    if (fabs(app->dsp.nco_shift_hz) > 1e-9) {
        char shift_buf[64];
        snprintf(shift_buf, sizeof(shift_buf), "%+.2f Hz%s", app->dsp.nco_shift_hz, config->dsp.shift_after_resample ? " (Post-Resample)" : "");
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Frequency Shift", shift_buf);
    }

    if (config->dsp.filter.count == 0) {
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Filter", "Disabled");
    } else {
        const char* filter_label;
        switch (app->dsp.filter.type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
            case FILTER_IMPL_FIR_ASYMMETRIC:
                filter_label = "FIR Filter";
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
            case FILTER_IMPL_FFT_ASYMMETRIC:
                filter_label = "FFT Filter";
                break;
            default:
                filter_label = "Filter";
                break;
        }

        char filter_buf[256] = {0};
        const char* stage = config->dsp.filter.apply_post_resample ? " (Post-Resample)" : "";
        strncat(filter_buf, "Enabled: ", sizeof(filter_buf) - strlen(filter_buf) - 1);
        for (int i = 0; i < config->dsp.filter.count; i++) {
            char current_filter_desc[128];
            const FilterRequest* req = &config->dsp.filter.requests[i];
            switch (req->type) {
                case FILTER_TYPE_LOWPASS: snprintf(current_filter_desc, sizeof(current_filter_desc), "LPF(%.15g Hz)", req->freq1_hz); break;
                case FILTER_TYPE_HIGHPASS: snprintf(current_filter_desc, sizeof(current_filter_desc), "HPF(%.15g Hz)", req->freq1_hz); break;
                case FILTER_TYPE_PASSBAND: snprintf(current_filter_desc, sizeof(current_filter_desc), "BPF(%.15g Hz, BW %.15g Hz)", req->freq1_hz, req->freq2_hz); break;
                case FILTER_TYPE_STOPBAND: snprintf(current_filter_desc, sizeof(current_filter_desc), "BSF(%.15g Hz, BW %.15g Hz)", req->freq1_hz, req->freq2_hz); break;
                default: break;
            }
            if (i > 0) strncat(filter_buf, " + ", sizeof(filter_buf) - strlen(filter_buf) - 1);
            strncat(filter_buf, current_filter_desc, sizeof(filter_buf) - strlen(filter_buf) - 1);
        }
        strncat(filter_buf, stage, sizeof(filter_buf) - strlen(filter_buf) - 1);
        fprintf(stderr, " %-*s : %s\n", max_label_len, filter_label, filter_buf);
    }

    if (config->dsp.agc.enable) {
        char agc_buf[128];
        snprintf(agc_buf, sizeof(agc_buf), "Enabled (Target: %.2f)", config->dsp.agc.target_level);
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Output AGC", agc_buf);
    } else {
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Output AGC", "Disabled");
    }

    fprintf(stderr, " %-*s : %s\n", max_label_len, "Resampling", app->dsp.bypass_resampler ? "Disabled" : "Enabled");

    if (config->output.path_arg != NULL) {
        const char* out_path;
#ifdef _WIN32
        out_path = config->output.effective_path_utf8;
#else
        out_path = config->output.effective_path;
#endif
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Output File", out_path);
    } else if (config->output.payload == PAYLOAD_AUDIO) {
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Output Target", "Audio Device");
    }
}

static void print_final_summary(const AppConfig *config, const AppContext* app, bool success) {
    (void)config;

    // If the output target is not a file (e.g., stdout), don't print a summary.
    if (config->output.path_arg == NULL) {
        return;
    }

    const int label_width = 32;
    char size_buf[40];
    char duration_buf[40];

    utils_format_size(app->stats.final_output_size_bytes, size_buf, sizeof(size_buf));
    double duration_secs = difftime(time(NULL), app->stats.start_time);
    utils_format_duration(duration_secs, duration_buf, sizeof(duration_buf));

    unsigned long long total_input_samples = (unsigned long long)atomic_load(&app->stats.total_frames_read) * 2;
    unsigned long long total_output_samples = (unsigned long long)atomic_load(&app->stats.total_output_frames) * 2;

    double avg_write_speed_mbps = 0.0;
    if (duration_secs > 0.001) {
        avg_write_speed_mbps = (double)app->stats.final_output_size_bytes / (1024.0 * 1024.0) / duration_secs;
    }

    fprintf(stderr, "\n--- Final Summary ---\n");
    if (!success) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Stopped Due to Error");
        if ((unsigned long long)atomic_load(&app->stats.total_frames_read) > 0) {
            log_error("Processing stopped after %llu input frames.", (unsigned long long)atomic_load(&app->stats.total_frames_read));
        }
        fprintf(stderr, "%-*s %s (possibly incomplete)\n", label_width, "Output File Size:", size_buf);
    } else if (app->stats.end_of_stream_reached) {
        fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Completed Successfully");
        fprintf(stderr, "%-*s %s\n", label_width, "Processing Duration:", duration_buf);
        fprintf(stderr, "%-*s %llu / %lld (100.0%%)\n", label_width, "Input Frames Read:", (unsigned long long)atomic_load(&app->stats.total_frames_read), (long long)app->module.source_info.frames);
        fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", (unsigned long long)atomic_load(&app->stats.total_output_frames));
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Samples Written:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", label_width, "Final Output Size:", size_buf);
        fprintf(stderr, "%-*s %.2f MB/s\n", label_width, "Average Write Speed:", avg_write_speed_mbps);
    } else if (is_shutdown_requested()) {
        bool source_has_known_length = (app->pipeline_mode == PIPELINE_MODE_FILE_PROCESSING);
        if (!source_has_known_length) {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Capture Stopped by User");
        } else {
            fprintf(stderr, "%-*s %s\n", label_width, "Status:", "Processing Cancelled by User");
        }
        const char* duration_label = !source_has_known_length ? "Capture Duration:" : "Processing Duration:";
        fprintf(stderr, "%-*s %s\n", label_width, duration_label, duration_buf);
        if (!source_has_known_length) {
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Frames Read:", (unsigned long long)atomic_load(&app->stats.total_frames_read));
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        } else {
            double percentage = 0.0;
            if (app->module.source_info.frames > 0) {
                percentage = ((double)(unsigned long long)atomic_load(&app->stats.total_frames_read) / (double)app->module.source_info.frames) * 100.0;
            }
            fprintf(stderr, "%-*s %llu / %lld (%.1f%%)\n", label_width, "Input Frames Read:", (unsigned long long)atomic_load(&app->stats.total_frames_read), (long long)app->module.source_info.frames, percentage);
            fprintf(stderr, "%-*s %llu\n", label_width, "Input Samples Read:", total_input_samples);
        }
        fprintf(stderr, "%-*s %llu\n", label_width, "Output Frames Written:", (unsigned long long)atomic_load(&app->stats.total_output_frames));
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

    if (CONSOLE_UPDATE_INTERVAL_SEC == 0) {
        return;
    }

    static double last_progress_log_time = 0.0;
    static long long last_bytes_written = 0;

    double current_time = utils_get_time();

    if (current_time - last_progress_log_time >= CONSOLE_UPDATE_INTERVAL_SEC) {
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
