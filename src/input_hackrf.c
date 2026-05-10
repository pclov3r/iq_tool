#include "input_hackrf.h"
#include "module.h"
#include "constants.h"
#include "app_context.h"
#include "signal_handler.h"
#include "log.h"
#include "freq_shift.h"
#include "utils.h"
#include "sample_format_table.h"
#include "sample_convert.h"
#include "input_common.h"
#include "mem_arena.h"
#include "argparse.h"
#include "wait_event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>

// Module-specific includes
#include <hackrf.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#include <strings.h>
#endif

extern pthread_mutex_t g_console_mutex;


// --- Private Module Configuration ---
static struct {
    uint32_t lna_gain;
    bool lna_gain_provided;
    int hackrf_lna_gain_arg;
    uint32_t vga_gain;
    bool vga_gain_provided;
    int hackrf_vga_gain_arg;
    bool amp_enable;
} s_hackrf_config;

// --- Private Module State ---
typedef struct {
    hackrf_device* dev;
    pthread_mutex_t driver_mutex;
} HackrfContext;


void hackrf_set_default_config(AppConfig* config) {
    config->sdr_general.sample_rate_hz = HACKRF_DEFAULT_SAMPLE_RATE;
    s_hackrf_config.lna_gain = HACKRF_DEFAULT_LNA_GAIN;
    s_hackrf_config.hackrf_lna_gain_arg = HACKRF_DEFAULT_LNA_GAIN;
    s_hackrf_config.vga_gain = HACKRF_DEFAULT_VGA_GAIN;
    s_hackrf_config.hackrf_vga_gain_arg = HACKRF_DEFAULT_VGA_GAIN;
}

static const struct argparse_option hackrf_input_cli_options[] = {
    OPT_GROUP("HackRF Input (hackrf)"),
    OPT_INTEGER(0, "hackrf-lna-gain", &s_hackrf_config.hackrf_lna_gain_arg, "Set LNA (IF) gain in dB. (Optional, Default: 16)", NULL, 0, 0),
    OPT_INTEGER(0, "hackrf-vga-gain", &s_hackrf_config.hackrf_vga_gain_arg, "Set VGA (Baseband) gain in dB. (Optional, Default: 0)", NULL, 0, 0),
    OPT_BOOLEAN(0, "hackrf-amp-enable", &s_hackrf_config.amp_enable, "Enable the front-end RF amplifier (+14 dB).", NULL, 0, 0),
};

const struct argparse_option* hackrf_input_get_cli_options(int* count) {
    *count = sizeof(hackrf_input_cli_options) / sizeof(hackrf_input_cli_options[0]);
    return hackrf_input_cli_options;
}

static bool hackrf_input_initialize(ModuleContext* ctx);
static void* hackrf_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx);
static void hackrf_input_stop_stream(ModuleContext* ctx);
static void hackrf_input_cleanup(ModuleContext* ctx);
static void hackrf_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool hackrf_input_validate_options(AppConfig* config);
static bool hackrf_input_validate_generic_options(const AppConfig* config);
static int hackrf_input_buffered_stream_callback(hackrf_transfer* transfer);


static InputModuleInterface s_hackrf_input_api = {
    .initialize = hackrf_input_initialize,
    .start_stream = hackrf_input_start_stream,
    .stop_stream = hackrf_input_stop_stream,
    .cleanup = hackrf_input_cleanup,
    .get_summary_info = hackrf_input_get_summary_info,
    .validate_options = hackrf_input_validate_options,
    .validate_generic_options = hackrf_input_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_hackrf_get_module_api(void) {
    return &s_hackrf_input_api;
}

static bool hackrf_input_validate_generic_options(const AppConfig* config) {
    if (!config->sdr_general.rf_freq_provided) {
        log_error("HackRF input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool hackrf_input_validate_options(AppConfig* config) {
    if (s_hackrf_config.hackrf_lna_gain_arg != HACKRF_DEFAULT_LNA_GAIN) {
        int lna_gain = s_hackrf_config.hackrf_lna_gain_arg;
        if (lna_gain < 0 || lna_gain > 40 || (lna_gain % 8 != 0)) {
            log_error("Invalid LNA gain %ld dB. Must be 0-40 in 8 dB steps.", lna_gain);
            return false;
        }
        s_hackrf_config.lna_gain = (uint32_t)lna_gain;
        s_hackrf_config.lna_gain_provided = true;
    }

    if (s_hackrf_config.hackrf_vga_gain_arg != HACKRF_DEFAULT_VGA_GAIN) {
        int vga_gain = s_hackrf_config.hackrf_vga_gain_arg;
        if (vga_gain < 0 || vga_gain > 62 || (vga_gain % 2 != 0)) {
            log_error("Invalid VGA gain %ld dB. Must be 0-62 in 2 dB steps.", vga_gain);
            return false;
        }
        s_hackrf_config.vga_gain = (uint32_t)vga_gain;
        s_hackrf_config.vga_gain_provided = true;
    }

    if (config->sdr_general.sample_rate_provided) {
        if (config->sdr_general.sample_rate_hz < 2e6 || config->sdr_general.sample_rate_hz > 20e6) {
            log_error("Invalid HackRF sample rate %.0f Hz. Must be between 2,000,000 and 20,000,000.", config->sdr_general.sample_rate_hz);
            return false;
        }
    }

    return true;
}

static int hackrf_input_buffered_stream_callback(hackrf_transfer* transfer) {
    AppContext* app = (AppContext*)transfer->rx_ctx;

    // --- HEARTBEAT ---

    if (is_shutdown_requested() || app->stats.error_occurred) {
        return -1;
    }

    // --- NEW ARCHITECTURE: DUMP THE WHOLE BLOCK ---
    // HackRF provides interleaved CS8 (2 bytes per sample).
    // valid_length is in bytes.
    if (!app->module.queue_samples(app->module.pipeline_ctx, // num_samples
            transfer->buffer, transfer->valid_length / 2, CS8)) {
        /* Warning handled internally by pipeline */
    }

    return 0;
}


static void hackrf_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppContext* app = ctx->app;
    add_summary_item(info, "Input Source", "HackRF One");
    add_summary_item(info, "Input Format", "8-bit Signed Complex (cs8)");
    add_summary_item(info, "Input Rate", "%d Hz", app->module.source_info.sample_rate);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr_general.rf_freq_hz);

    // as HackRF does not have a true hardware AGC. The gain is always fixed.
    add_summary_item(info, "LNA Gain", "%u dB", s_hackrf_config.lna_gain);
    add_summary_item(info, "VGA Gain", "%u dB", s_hackrf_config.vga_gain);
    add_summary_item(info, "RF Amp", "%s", s_hackrf_config.amp_enable ? "Enabled" : "Disabled");
    add_summary_item(info, "Bias-T", "%s", config->sdr_general.bias_t_enable ? "Enabled" : "Disabled");
}

static bool hackrf_input_initialize(ModuleContext* ctx) {
    const AppConfig *config = ctx->config;
    AppContext* app = ctx->app;
    int result;
    bool success = false; // Assume failure until the very end

    HackrfContext* private_data = (HackrfContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(HackrfContext), true);
    if (!private_data) {
        return false; // mem_arena_alloc logs error, no app to clean up yet
    }
    private_data->dev = NULL;

    if (pthread_mutex_init(&private_data->driver_mutex, NULL) != 0) {
        log_error("Failed to init driver mutex.");
        return false;
    } // Initialize resource state
    app->module.input_private_data = private_data;

    result = hackrf_init();
    if (result != HACKRF_SUCCESS) {
        log_error("hackrf_init() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup; // On error, jump to the single cleanup point
    }

    result = hackrf_open(&private_data->dev);
    if (result != HACKRF_SUCCESS) {
        log_error("hackrf_open() failed: %s (%d)", hackrf_error_name(result), result);
        private_data->dev = NULL; // Ensure dev is NULL on failure
        goto cleanup;
    }
    log_info("Found HackRF One.");

    result = hackrf_set_sample_rate(private_data->dev, config->sdr_general.sample_rate_hz);
    if (result != HACKRF_SUCCESS) {
        log_error("hackrf_set_sample_rate() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_freq(private_data->dev, (uint64_t)config->sdr_general.rf_freq_hz);
    if (result != HACKRF_SUCCESS) {
        log_error("hackrf_set_freq() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_lna_gain(private_data->dev, s_hackrf_config.lna_gain);
    if (result != HACKRF_SUCCESS) {
        log_error("hackrf_set_lna_gain() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_vga_gain(private_data->dev, s_hackrf_config.vga_gain);
    if (result != HACKRF_SUCCESS) {
        log_error("hackrf_set_vga_gain() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_amp_enable(private_data->dev, (uint8_t)s_hackrf_config.amp_enable);
    if (result != HACKRF_SUCCESS) {
        log_error("hackrf_set_amp_enable() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    if (config->sdr_general.bias_t_enable) {
        result = hackrf_set_antenna_enable(private_data->dev, 1);
        if (result != HACKRF_SUCCESS) {
            log_error("hackrf_set_antenna_enable() failed: %s (%d)", hackrf_error_name(result), result);
            goto cleanup;
        }
    }

    app->module.input_format = CS8;
    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(app->module.input_format);
    app->module.source_info.sample_rate = (int)config->sdr_general.sample_rate_hz;
    app->module.source_info.frames = -1;


    app->pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT;
    success = true;

cleanup:
    if (!success) {
        // The main application cleanup will call hackrf_input_cleanup(), which handles these.
    }
    return success;
}

static void* hackrf_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx) {
    ctx->app->module.queue_samples = queue_samples;
    ctx->app->module.pipeline_ctx = pipeline_ctx;
    AppContext* app = ctx->app;
    HackrfContext* private_data = (HackrfContext*)app->module.input_private_data;
    int result;
    hackrf_sample_block_cb_fn callback_fn;
    log_info("Starting hackrf stream...");
    callback_fn = hackrf_input_buffered_stream_callback;

    result = hackrf_start_rx(private_data->dev, callback_fn, app);
    if (result != HACKRF_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hackrf_start_rx() failed: %s (%d)", hackrf_error_name(result), result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    // Wait for the shutdown signal (Event-driven)
    if (app->pipeline.shutdown_event) {
        wait_event_wait(app->pipeline.shutdown_event);
    }

    if (!is_shutdown_requested()) {
        hackrf_input_stop_stream(ctx);
    }

    return NULL;
}

static void hackrf_input_stop_stream(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    HackrfContext* private_data = (HackrfContext*)app->module.input_private_data;
    if (private_data) {
    pthread_mutex_lock(&private_data->driver_mutex);
    if (private_data && private_data->dev && hackrf_is_streaming(private_data->dev) == HACKRF_TRUE) {
        log_info("Stopping HackRF stream...");
        int result = hackrf_stop_rx(private_data->dev);
        if (result != HACKRF_SUCCESS) {
            log_error("Failed to stop HackRF RX: %s (%d)", hackrf_error_name(result), result);
        }
    }
    pthread_mutex_unlock(&private_data->driver_mutex);
}
}

static void hackrf_input_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (app->module.input_private_data) {
        HackrfContext* private_data = (HackrfContext*)app->module.input_private_data;
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->dev) {
            log_info("Closing HackRF device...");
            hackrf_close(private_data->dev);
            private_data->dev = NULL;
        }
        pthread_mutex_unlock(&private_data->driver_mutex);
        pthread_mutex_destroy(&private_data->driver_mutex);
        app->module.input_private_data = NULL;
    }
    log_info("Exiting HackRF library...");
    hackrf_exit();
}
