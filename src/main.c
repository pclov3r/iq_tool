/**
 * @file main.c
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
#include "utilities.h"
#include "module_registry.h"
#include "presets_loader.h"
#include "platform.h"
#include "mem_arena.h"
#include "keyboard_handler.h"
#include "sample_format_table.h"
#include "pipeline.h"
#include "filter.h"
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
static void print_summary_section(const char* header, const InputSummaryInfo* info, int w);
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
    PipelineContext pipeline_context = {0};
    bool resources_initialized = false;
    bool arena_initialized = false;

    AppConfig config;
    memset(&config, 0, sizeof(AppConfig));

    int return_code;

    pthread_mutexattr_t attr;
    if ((return_code = pthread_mutexattr_init(&attr)) != 0) {
        fprintf(stderr, "FATAL: Failed to initialize mutex attributes: %s\n", strerror(return_code));
        return EXIT_FAILURE;
    }
    if ((return_code = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) != 0) {
        fprintf(stderr, "FATAL: Failed to set mutex type to recursive: %s\n", strerror(return_code));
        pthread_mutexattr_destroy(&attr);
        return EXIT_FAILURE;
    }
    if ((return_code = pthread_mutex_init(&g_console_mutex, &attr)) != 0) {
        fprintf(stderr, "FATAL: Failed to initialize console mutex: %s\n", strerror(return_code));
        pthread_mutexattr_destroy(&attr);
        return EXIT_FAILURE;
    }
    pthread_mutexattr_destroy(&attr);

    log_set_lock(console_lock_function, &g_console_mutex);
    log_set_level(LOG_INFO);

    // Verify CPU hardware requirements immediately before starting
    platform_check_cpu_features();

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

    if (!cli_parse(argc, argv, &app)) {
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
        ModuleContext context = { .config = &config, .app = &app };
        if (!app.module.input_api->pre_stream_iq_correction(&context)) goto cleanup;
    }
    pipeline_context.config = &config;
    pipeline_context.app = &app;
    if (!pipeline_setup_buffers(&pipeline_context)) {
        goto cleanup;
    }

    if (!init_output_module(&config, &app)) {
        goto cleanup;
    }
    resources_initialized = true;

    print_configuration_summary(&config, &app);

    if (app.pipeline_mode == PIPELINE_MODE_ASYNCHRONOUS_PUSH) {
        log_info("Starting %s live source capture...", config.input.type_name);
    } else {
        log_info("Starting %s file processing...", config.input.type_name);
    }

    app.stats.progress_callback = application_progress_callback;
    app.stats.progress_callback_udata = &g_console_mutex;

    app.stats.start_time = time(NULL);

    setup_keyboard_handler(&app);

    if (!pipeline_run(&pipeline_context)) {
        log_fatal("Pipeline execution failed.");
    }

    bool processing_ok = !atomic_load_explicit(&app.stats.error_occurred, memory_order_relaxed);
    exit_status = processing_ok ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    pthread_mutex_lock(&g_console_mutex);

    bool final_ok = !app.stats.error_occurred;

    if (app.module.input_api) {
        if (app.pipeline_mode == PIPELINE_MODE_ASYNCHRONOUS_PUSH) {
            log_info("Stopping %s live source capture...", config.input.type_name);
        } else {
            log_info("Finished %s file processing.", config.input.type_name);
        }
        log_info("Closing %s input module...", config.input.type_name);
        close_input_source(&config, &app);
    }

    if (app.module.output_api) {
        log_info("Closing %s output module...", config.output.module_name);
        close_output_module(&config, &app);
    }

    if (resources_initialized) {
        print_final_summary(&config, &app, final_ok);
    }

    pthread_mutex_unlock(&g_console_mutex);

    // Always tear down pipeline buffers and components before freeing their memory arena
    pipeline_teardown(&pipeline_context);

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
    ModuleContext context = { .config = config, .app = app };

    const Module* selected_input_module = module_get(config->input.type_name, MODULE_TYPE_INPUT, &app->pipeline.setup_arena);
    if (!selected_input_module) {
        log_error("Input type '%s' is not supported or enabled in this build.", config->input.type_name);
        return false;
    }
    app->module.input_api = (InputModuleInterface*)selected_input_module->api;
    app->pipeline_mode = selected_input_module->pipeline_mode;

    app->module.source_info.demod_audio_buffer_size = selected_input_module->default_demod_audio_buffer_size;

    log_info("Initializing the '%s' input module...", config->input.type_name);
    return app->module.input_api->initialize(&context);
}

static bool init_output_module(AppConfig *config, AppContext* app) {
    ModuleContext context = { .config = config, .app = app };

    const Module* selected_output_module = module_get(config->output.module_name, MODULE_TYPE_OUTPUT, &app->pipeline.setup_arena);
    if (!selected_output_module) {
        log_fatal("Internal error: Could not retrieve selected output module.");
        return false;
    }
    app->module.output_api = (OutputModuleInterface*)selected_output_module->api;

    if (config->dsp.raw_passthrough && app->module.input_format != app->dsp.pipeline_sample_format) {
        log_error("Option --raw-passthrough requires input and output formats to be identical.");
        return false;
    }

    log_info("Initializing the '%s' output module...", config->output.module_name);
    return app->module.output_api->initialize(&context);
}

static void close_input_source(AppConfig *config, AppContext* app) {
    if (!app || !app->module.input_api) return;
    ModuleContext context = { .config = config, .app = app };
    if (app->module.input_api->cleanup) {
        app->module.input_api->cleanup(&context);
    }
}

static void close_output_module(AppConfig *config, AppContext* app) {
    if (!app || !app->module.output_api) return;
    ModuleContext context = { .config = config, .app = app };
    if (app->module.output_api->cleanup) {
        app->module.output_api->cleanup(&context);
    }
}

// --- Static Helper Function Definitions ---

static void print_summary_section(const char* header, const InputSummaryInfo* info, int w) {
    fprintf(stderr, "--- %s ---\n", header);
    for (int i = 0; i < info->count; i++)
        fprintf(stderr, " %-*s : %s\n", w, info->items[i].label, info->items[i].value);
}

static void initialize_resource_struct(AppConfig *config, AppContext* app) {
    memset(app, 0, sizeof(AppContext));
    app->config = config;

    // Set global DSP defaults
    config->dsp.input_gain = 1.0f;
    config->dsp.output_gain = 1.0f;
    config->dsp.baseband_gain = 1.0f;

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

    const ModuleContext context = { .config = config, .app = (AppContext*)app };

    // --- Build input section ---
    InputSummaryInfo input_info = {0};
    app->module.input_api->get_summary_info(&context, &input_info);

    if (config->sdr_general.rf_freq_provided) {
        if (fabs(config->sdr_general.frequency_offset_hz) > 1e-9) {
            double user_hz = config->sdr_general.rf_freq_hz - config->sdr_general.frequency_offset_hz;
            add_summary_item(&input_info, "Actual Frequency", "%.15g Hz", user_hz);
            add_summary_item(&input_info, "Frequency Offset", "%+.0f Hz", config->sdr_general.frequency_offset_hz);
            add_summary_item(&input_info, "Tuned Frequency",  "%.15g Hz", config->sdr_general.rf_freq_hz);
        } else {
            add_summary_item(&input_info, "RF Frequency", "%.15g Hz", config->sdr_general.rf_freq_hz);
        }
    }
    add_summary_item(&input_info, "I/Q Correction", "%s", config->dsp.iq_correction.enable ? "Enabled" : "Disabled");
    add_summary_item(&input_info, "DC Block",        "%s", config->dsp.dc_block.enable      ? "Enabled" : "Disabled");

    // --- Build output section ---
    OutputSummaryInfo output_info = {0};
    if (app->module.output_api && app->module.output_api->get_summary_info)
        app->module.output_api->get_summary_info(&context, &output_info);

    const char* sample_type_str = get_format_info_by_enum(app->dsp.pipeline_sample_format)
                                ? get_format_info_by_enum(app->dsp.pipeline_sample_format)->description_str
                                : "Unknown";
    add_summary_item(&output_info, "Sample Type", "%s", sample_type_str);

    if (config->output.payload == PAYLOAD_AUDIO) {
        add_summary_item(&output_info, "Baseband Sample Rate", "%.15g Hz", config->baseband_sample_rate.rate_hz);
    } else {
        add_summary_item(&output_info, "Output Sample Rate",   "%.15g Hz", config->output_sample_rate.rate_hz);
    }

    add_summary_item(&output_info, "Input Gain",  "%.5f", config->dsp.input_gain);
    if (config->dsp.output_gain != 1.0f)
        add_summary_item(&output_info, "Output Gain", "%.5f", config->dsp.output_gain);

    if (fabs(app->dsp.nco_shift_hz) > 1e-9)
        add_summary_item(&output_info, "Frequency Shift", "%+.2f Hz%s",
            app->dsp.nco_shift_hz, config->dsp.shift_after_resample ? " (Post-Resample)" : "");

    filter_get_summary_info(config, app, &output_info);

    if (app->dsp.pipeline_agc.enable) {
        add_summary_item(&output_info, "Pipeline AGC", "Enabled (Target: %.2f)", app->dsp.pipeline_agc.target_level);
    } else {
        add_summary_item(&output_info, "Pipeline AGC", "Disabled");
    }

    if (app->dsp.pipeline_gain != 1.0f)
        add_summary_item(&output_info, "Pipeline Gain", "%.2fx", app->dsp.pipeline_gain);

    add_summary_item(&output_info, "Resampling", "%s", app->dsp.bypass_resampler ? "Disabled" : "Enabled");

    if (config->output.path_arg != NULL) {
#ifdef _WIN32
        add_summary_item(&output_info, "Output File", "%s", config->output.effective_path_utf8);
#else
        add_summary_item(&output_info, "Output File", "%s", config->output.effective_path);
#endif
    } else if (config->output.payload == PAYLOAD_AUDIO) {
        add_summary_item(&output_info, "Output Target", "Audio Device");
    }

    // --- Single unified width pass ---
    int w = 0;
    for (int i = 0; i < input_info.count;  i++) { int l = (int)strlen(input_info.items[i].label);  if (l > w) w = l; }
    for (int i = 0; i < output_info.count; i++) { int l = (int)strlen(output_info.items[i].label); if (l > w) w = l; }

    // --- Render ---
    fprintf(stderr, "\n");
    print_summary_section("Input Details",  &input_info,  w);
    print_summary_section("Output Details", &output_info, w);
    fprintf(stderr, "\n");
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

    utility_format_size(app->stats.final_output_size_bytes, size_buf, sizeof(size_buf));
    double duration_secs = difftime(time(NULL), app->stats.start_time);
    utility_format_duration(duration_secs, duration_buf, sizeof(duration_buf));

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
        bool source_has_known_length = (app->pipeline_mode == PIPELINE_MODE_SYNCHRONOUS_PULL);
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

    double current_time = utility_get_time();

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
