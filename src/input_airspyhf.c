#include "input_airspyhf.h"
#include "module.h"
#include "sample_convert.h"
#include "constants.h"
#include "app_context.h"
#include "signal_handler.h"
#include "log.h"
#include "freq_shift.h"
#include "utils.h"
#include "sample_format_table.h"
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
#include <airspyhf.h>

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
    char* agc_mode;
    bool agc_mode_provided;
    float attenuation;
    bool attenuation_provided;
    bool preamp_enabled;
    bool preamp_provided;
    uint64_t serial_number;
    bool serial_provided;
    bool lib_dsp_disabled;
} s_airspyhf_config;

// --- Private Module State ---
typedef struct {
    struct airspyhf_device* dev;
    pthread_mutex_t driver_mutex;
} AirspyHFContext;


void airspyhf_set_default_config(AppConfig* config) {
    config->sdr_general.sample_rate_hz = AIRSPYHF_DEFAULT_SAMPLE_RATE;
}

static const struct argparse_option airspyhf_input_cli_options[] = {
    OPT_GROUP("Airspy HF+ Input (airspyhf)"),
    OPT_STRING(0, "airspyhf-agc", &s_airspyhf_config.agc_mode, "AGC mode: 'off', 'low', or 'high'. (Default: high)", NULL, 0, 0),
    OPT_FLOAT(0, "airspyhf-attn", &s_airspyhf_config.attenuation, "Attenuation in dB (0.0 to 48.0). (Default: 0.0)", NULL, 0, 0),
    OPT_BOOLEAN(0, "airspyhf-preamp", &s_airspyhf_config.preamp_enabled, "Enable LNA/PreAmp.", NULL, 0, 0),
    OPT_INTEGER(0, "airspyhf-serial", (int*)&s_airspyhf_config.serial_number, "Select device by serial number (hex, e.g., 0x123456789ABCDEF0).", NULL, 0, 0),
    OPT_BOOLEAN(0, "airspyhf-no-lib-dsp", &s_airspyhf_config.lib_dsp_disabled, "Disable library DSP processing (IQ correction, DC removal, etc).", NULL, 0, 0),
};

const struct argparse_option* airspyhf_input_get_cli_options(int* count) {
    *count = sizeof(airspyhf_input_cli_options) / sizeof(airspyhf_input_cli_options[0]);
    return airspyhf_input_cli_options;
}

static bool airspyhf_input_initialize(ModuleContext* ctx);
static void* airspyhf_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx);
static void airspyhf_input_stop_stream(ModuleContext* ctx);
static void airspyhf_input_cleanup(ModuleContext* ctx);
static void airspyhf_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool airspyhf_input_validate_options(AppConfig* config);
static bool airspyhf_input_validate_generic_options(const AppConfig* config);
static int airspyhf_input_buffered_stream_callback(airspyhf_transfer_t* transfer);


static InputModuleInterface s_airspyhf_input_api = {
    .initialize = airspyhf_input_initialize,
    .start_stream = airspyhf_input_start_stream,
    .stop_stream = airspyhf_input_stop_stream,
    .cleanup = airspyhf_input_cleanup,
    .get_summary_info = airspyhf_input_get_summary_info,
    .validate_options = airspyhf_input_validate_options,
    .validate_generic_options = airspyhf_input_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_airspyhf_get_module_api(void) {
    return &s_airspyhf_input_api;
}

static bool airspyhf_input_validate_generic_options(const AppConfig* config) {
    if (!config->sdr_general.rf_freq_provided) {
        log_error("Airspy HF+ input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool airspyhf_input_validate_options(AppConfig* config) {
    // AGC Mode Validation
    if (s_airspyhf_config.agc_mode) {
        s_airspyhf_config.agc_mode_provided = true;
        if (strcasecmp(s_airspyhf_config.agc_mode, "off") != 0 &&
            strcasecmp(s_airspyhf_config.agc_mode, "low") != 0 &&
            strcasecmp(s_airspyhf_config.agc_mode, "high") != 0) {
            log_error("Invalid --airspyhf-agc '%s'. Must be 'off', 'low', or 'high'.", s_airspyhf_config.agc_mode);
            return false;
        }
    }

    // Attenuation Validation
    if (s_airspyhf_config.attenuation != 0.0f) {
        s_airspyhf_config.attenuation_provided = true;
        if (s_airspyhf_config.attenuation < 0.0f || s_airspyhf_config.attenuation > 48.0f) {
            log_error("Invalid --airspyhf-attn %.1f. Must be between 0.0 and 48.0.", s_airspyhf_config.attenuation);
            return false;
        }
    }

    // Preamp Validation
    if (s_airspyhf_config.preamp_enabled) {
        s_airspyhf_config.preamp_provided = true;
    }

    // Serial Number Validation
    if (s_airspyhf_config.serial_number != 0) {
        s_airspyhf_config.serial_provided = true;
    }

    // Sample Rate Validation
    if (config->sdr_general.sample_rate_provided) {
        // Common Airspy HF+ sample rates
        static const uint32_t VALID_AIRSPYHF_RATES[] = {
            192000,   // 192 kHz
            228000,   // 228 kHz
            384000,   // 384 kHz
            456000,   // 456 kHz
            768000,   // 768 kHz
            912000    // 912 kHz
        };
        const size_t NUM_RATES = sizeof(VALID_AIRSPYHF_RATES) / sizeof(VALID_AIRSPYHF_RATES[0]);

        bool rate_found = false;
        uint32_t requested = (uint32_t)config->sdr_general.sample_rate_hz;

        for (size_t i = 0; i < NUM_RATES; i++) {
            if (requested == VALID_AIRSPYHF_RATES[i]) {
                rate_found = true;
                break;
            }
        }

        if (!rate_found) {
            log_error("Invalid Airspy HF+ sample rate %u Hz.", requested);
            log_error("Common sample rates: 192000, 228000, 384000, 456000, 768000, 912000");
            log_error("Note: Supported rates vary by device model. The device will report actual supported rates during initialization.");
            return false;
        }
    }

    // Warn if both AGC and manual attenuation are specified
    if (s_airspyhf_config.agc_mode_provided && 
        strcasecmp(s_airspyhf_config.agc_mode, "off") != 0 &&
        s_airspyhf_config.attenuation_provided) {
        log_warn("Both AGC and manual attenuation specified. Manual attenuation will be ignored when AGC is active.");
    }

    return true;
}

static int airspyhf_input_buffered_stream_callback(airspyhf_transfer_t* transfer) {
    AppContext* app = (AppContext*)transfer->ctx;

    // --- HEARTBEAT ---

    if (is_shutdown_requested() || app->stats.error_occurred) {
        return -1;
    }

    if (transfer->sample_count == 0) {
        return 0;
    }

    // Airspy HF+ always outputs CF32
    if (!app->module.queue_samples(app->module.pipeline_ctx, transfer->samples, transfer->sample_count, CF32)) {
        log_warn("SDR input buffer overrun! Dropped data.");
    }

    return 0;
}


static void airspyhf_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppContext* app = ctx->app;

    add_summary_item(info, "Input Source", "Airspy HF+");
    add_summary_item(info, "Input Format", "32-bit Float Complex (cf32)");
    add_summary_item(info, "Input Rate", "%d Hz", app->module.source_info.sample_rate);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr_general.rf_freq_hz);

    // Gain reporting
    if (s_airspyhf_config.agc_mode_provided) {
        if (strcasecmp(s_airspyhf_config.agc_mode, "off") == 0) {
            add_summary_item(info, "AGC", "Off");
            add_summary_item(info, "Attenuation", "%.1f dB", s_airspyhf_config.attenuation);
        } else if (strcasecmp(s_airspyhf_config.agc_mode, "low") == 0) {
            add_summary_item(info, "AGC", "Low Threshold");
        } else if (strcasecmp(s_airspyhf_config.agc_mode, "high") == 0) {
            add_summary_item(info, "AGC", "High Threshold");
        }
    } else {
        add_summary_item(info, "AGC", "High Threshold (Default)");
    }

    if (s_airspyhf_config.preamp_provided && s_airspyhf_config.preamp_enabled) {
        add_summary_item(info, "LNA/PreAmp", "Enabled");
    }

    if (s_airspyhf_config.lib_dsp_disabled) {
        add_summary_item(info, "Library DSP", "Disabled");
    }

    add_summary_item(info, "Bias-T", "%s", config->sdr_general.bias_t_enable ? "Enabled" : "Disabled");
}

static bool airspyhf_input_initialize(ModuleContext* ctx) {
    const AppConfig *config = ctx->config;
    AppContext* app = ctx->app;
    int result;
    bool success = false;

    AirspyHFContext* private_data = (AirspyHFContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(AirspyHFContext), true);
    if (!private_data) {
        return false;
    }
    private_data->dev = NULL;

    if (pthread_mutex_init(&private_data->driver_mutex, NULL) != 0) {
        log_error("Failed to init driver mutex.");
        return false;
    }

    app->module.input_private_data = private_data;

    // Open device
    if (s_airspyhf_config.serial_provided) {
        result = airspyhf_open_sn(&private_data->dev, s_airspyhf_config.serial_number);
    } else {
        result = airspyhf_open(&private_data->dev);
    }

    if (result != AIRSPYHF_SUCCESS) {
        log_error("airspyhf_open() failed: %d", result);
        private_data->dev = NULL;
        goto cleanup;
    }

    // Read 128-bit Serial Number (4 chunks of 32-bits)
    airspyhf_read_partid_serialno_t s = {0};
    airspyhf_board_partid_serialno_read(private_data->dev, &s);

    log_info("Using Airspy HF+ device (S/N: 0x%08X%08X%08X%08X)",
             s.serial_no[0], s.serial_no[1], s.serial_no[2], s.serial_no[3]);

    // Get and validate supported sample rates
    uint32_t num_rates = 0;
    result = airspyhf_get_samplerates(private_data->dev, &num_rates, 0);
    if (result != AIRSPYHF_SUCCESS) {
        log_error("Failed to get number of supported sample rates: %d", result);
        goto cleanup;
    }

    uint32_t* rates = (uint32_t*)mem_arena_alloc(&app->pipeline.setup_arena, num_rates * sizeof(uint32_t), false);
    if (!rates) {
        goto cleanup;
    }

    result = airspyhf_get_samplerates(private_data->dev, rates, num_rates);
    if (result != AIRSPYHF_SUCCESS) {
        log_error("Failed to get supported sample rates: %d", result);
        goto cleanup;
    }

    // Find if requested rate is supported
    bool rate_supported = false;
    uint32_t requested_rate = (uint32_t)config->sdr_general.sample_rate_hz;
    for (uint32_t i = 0; i < num_rates; i++) {
        if (rates[i] == requested_rate) {
            rate_supported = true;
            break;
        }
    }

    if (!rate_supported) {
        log_error("Requested sample rate %.0f Hz is not supported by this Airspy HF+ device.", config->sdr_general.sample_rate_hz);
        log_error("Supported rates are:");
        for (uint32_t i = 0; i < num_rates; i++) {
            log_error("  %u Hz", rates[i]);
        }
        goto cleanup;
    }

    // Set sample rate
    result = airspyhf_set_samplerate(private_data->dev, requested_rate);
    if (result != AIRSPYHF_SUCCESS) {
        log_error("airspyhf_set_samplerate() failed: %d", result);
        goto cleanup;
    }

    // Set frequency
    result = airspyhf_set_freq(private_data->dev, (uint32_t)config->sdr_general.rf_freq_hz);
    if (result != AIRSPYHF_SUCCESS) {
        log_error("airspyhf_set_freq() failed: %d", result);
        goto cleanup;
    }

    // Airspy HF+ always outputs CF32
    app->module.input_format = CF32;

    // Configure AGC
    if (s_airspyhf_config.agc_mode_provided) {
        if (strcasecmp(s_airspyhf_config.agc_mode, "off") == 0) {
            log_info("Disabling AGC (manual attenuation control)...");
            result = airspyhf_set_hf_agc(private_data->dev, 0);
            if (result != AIRSPYHF_SUCCESS) {
                log_error("airspyhf_set_hf_agc() failed: %d", result);
                goto cleanup;
            }
            // Set manual attenuation
            result = airspyhf_set_hf_att(private_data->dev, (uint8_t)(s_airspyhf_config.attenuation / 6.0f));
            if (result != AIRSPYHF_SUCCESS) {
                log_error("airspyhf_set_hf_att() failed: %d", result);
                goto cleanup;
            }
        } else {
            // Enable AGC
            const char* mode_desc = (strcasecmp(s_airspyhf_config.agc_mode, "high") == 0) ? "High" : "Low";
            log_info("Enabling AGC (%s Threshold)...", mode_desc);

            result = airspyhf_set_hf_agc(private_data->dev, 1);
            if (result != AIRSPYHF_SUCCESS) {
                log_error("airspyhf_set_hf_agc() failed: %d", result);
                goto cleanup;
            }
            // Set AGC threshold
            uint8_t threshold = (strcasecmp(s_airspyhf_config.agc_mode, "high") == 0) ? 1 : 0;
            result = airspyhf_set_hf_agc_threshold(private_data->dev, threshold);
            if (result != AIRSPYHF_SUCCESS) {
                log_error("airspyhf_set_hf_agc_threshold() failed: %d", result);
                goto cleanup;
            }
        }
    } else {
        // Default: Enable AGC with high threshold
        log_info("Enabling AGC (default: high threshold)...");
        result = airspyhf_set_hf_agc(private_data->dev, 1);
        if (result != AIRSPYHF_SUCCESS) {
            log_error("airspyhf_set_hf_agc() failed: %d", result);
            goto cleanup;
        }
        result = airspyhf_set_hf_agc_threshold(private_data->dev, 1);
        if (result != AIRSPYHF_SUCCESS) {
            log_error("airspyhf_set_hf_agc_threshold() failed: %d", result);
            goto cleanup;
        }
    }

    // Configure LNA/PreAmp
    if (s_airspyhf_config.preamp_provided && s_airspyhf_config.preamp_enabled) {
        log_info("Enabling LNA/PreAmp...");
        result = airspyhf_set_hf_lna(private_data->dev, 1);
        if (result != AIRSPYHF_SUCCESS) {
            log_error("airspyhf_set_hf_lna() failed: %d", result);
            goto cleanup;
        }
    } else {
        result = airspyhf_set_hf_lna(private_data->dev, 0);
        if (result != AIRSPYHF_SUCCESS) {
            log_error("airspyhf_set_hf_lna() failed: %d", result);
            goto cleanup;
        }
    }

    // Configure Library DSP
    if (s_airspyhf_config.lib_dsp_disabled) {
        log_info("Disabling library DSP processing...");
        result = airspyhf_set_lib_dsp(private_data->dev, 0);
        if (result != AIRSPYHF_SUCCESS) {
            log_error("airspyhf_set_lib_dsp() failed: %d", result);
            goto cleanup;
        }
    } else {
        result = airspyhf_set_lib_dsp(private_data->dev, 1);
        if (result != AIRSPYHF_SUCCESS) {
            log_error("airspyhf_set_lib_dsp() failed: %d", result);
            goto cleanup;
        }
    }

    // Set Bias-T if requested
    if (config->sdr_general.bias_t_enable) {
        log_info("Enabling Bias-T...");
        result = airspyhf_set_user_output(private_data->dev, AIRSPYHF_USER_OUTPUT_0, AIRSPYHF_USER_OUTPUT_HIGH);
        if (result != AIRSPYHF_SUCCESS) {
            log_warn("Failed to enable Bias-T: %d (this device may not support bias-T)", result);
        }
    }

    app->module.input_bytes_per_sample_pair = (get_format_info_by_enum(CF32) ? get_format_info_by_enum(CF32)->bytes_per_pair : 0);
    app->module.source_info.sample_rate = (int)config->sdr_general.sample_rate_hz;
    app->module.source_info.frames = -1;


    app->pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT;
    success = true;

cleanup:
    if (!success) {
        // Cleanup will be handled by airspyhf_input_cleanup()
    }
    return success;
}

static void* airspyhf_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx) {
    ctx->app->module.queue_samples = queue_samples;
    ctx->app->module.pipeline_ctx = pipeline_ctx;
    AppContext* app = ctx->app;
    AirspyHFContext* private_data = (AirspyHFContext*)app->module.input_private_data;
    int result;
    airspyhf_sample_block_cb_fn callback_fn;
    log_info("Starting airspyhf stream...");
    callback_fn = airspyhf_input_buffered_stream_callback;

    result = airspyhf_start(private_data->dev, callback_fn, app);
    if (result != AIRSPYHF_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "airspyhf_start() failed: %d", result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    // Wait for the shutdown signal (Event-driven)
    if (app->pipeline.shutdown_event) {
        wait_event_wait(app->pipeline.shutdown_event);
    }

    if (!is_shutdown_requested()) {
        airspyhf_input_stop_stream(ctx);
    }

    return NULL;
}

static void airspyhf_input_stop_stream(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    AirspyHFContext* private_data = (AirspyHFContext*)app->module.input_private_data;
    if (private_data) {
    pthread_mutex_lock(&private_data->driver_mutex);
    if (private_data && private_data->dev && airspyhf_is_streaming(private_data->dev)) {
        log_info("Stopping Airspy HF+ stream...");
        int result = airspyhf_stop(private_data->dev);
        if (result != AIRSPYHF_SUCCESS) {
            log_error("Failed to stop Airspy HF+ RX: %d", result);
        }
    }
    pthread_mutex_unlock(&private_data->driver_mutex);
}
}

static void airspyhf_input_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (app->module.input_private_data) {
        AirspyHFContext* private_data = (AirspyHFContext*)app->module.input_private_data;
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->dev) {
            log_info("Closing Airspy HF+ device...");
            airspyhf_close(private_data->dev);
            private_data->dev = NULL;
        }
        pthread_mutex_unlock(&private_data->driver_mutex);
        pthread_mutex_destroy(&private_data->driver_mutex);
        app->module.input_private_data = NULL;
    }
}
