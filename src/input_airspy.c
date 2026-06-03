#include "input_airspy.h"
#include "module.h"
#include "sample_convert.h"
#include "constants.h"
#include "app_context.h"
#include "signal_handler.h"
#include "log.h"
#include "frequency_shift.h"
#include "utilities.h"
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
#include <airspy.h>

// --- Fix for Missing Board IDs ---
// The official airspy.h only defines AIRSPY_BOARD_ID_PROTO_AIRSPY (0).
// We map the hardware IDs locally to allow identifying R2 vs Mini.
#ifndef AIRSPY_BOARD_ID_AIRSPY_R2
#define AIRSPY_BOARD_ID_AIRSPY_R2 0
#endif

#ifndef AIRSPY_BOARD_ID_AIRSPY_MINI
#define AIRSPY_BOARD_ID_AIRSPY_MINI 1
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#include <strings.h>
#endif

extern pthread_mutex_t g_console_mutex;

// Max expected buffer size for unpacking.
// Airspy USB transfers are typically 128KB - 256KB.
// We'll allocate enough space for 256K complex samples (16-bit I/Q) to be safe.
// 262144 samples * 4 bytes/sample = ~1MB.
#define AIRSPY_UNPACK_BUFFER_SAMPLES 262144

// --- Private Module Configuration ---
static struct {
    char* gain_mode;
    bool gain_mode_provided;
    int gain_value;
    bool gain_value_provided;
    int airspy_gain_value_arg;
    int lna_gain;
    bool lna_gain_provided;
    int airspy_lna_gain_arg;
    int mixer_gain;
    bool mixer_gain_provided;
    int airspy_mixer_gain_arg;
    int vga_gain;
    bool vga_gain_provided;
    int airspy_vga_gain_arg;
    char* sample_format;
    bool sample_format_provided;
    uint64_t serial_number;
    bool serial_provided;
    bool packing_enabled;
} s_airspy_config;

// --- Private Module State ---
typedef struct {
    struct airspy_device* dev;
    enum airspy_sample_type sample_type;
    const char* board_name;         // "Airspy R2", "Airspy Mini", etc.
    pthread_mutex_t driver_mutex;
} AirspyContext;


void airspy_set_default_config(AppConfig* config) {
    config->sdr_general.sample_rate_hz = AIRSPY_DEFAULT_SAMPLE_RATE;
    s_airspy_config.gain_value = AIRSPY_DEFAULT_GAIN_VALUE;
    s_airspy_config.airspy_gain_value_arg = AIRSPY_DEFAULT_GAIN_VALUE;
    s_airspy_config.lna_gain = AIRSPY_DEFAULT_LNA_GAIN;
    s_airspy_config.airspy_lna_gain_arg = AIRSPY_DEFAULT_LNA_GAIN;
    s_airspy_config.mixer_gain = AIRSPY_DEFAULT_MIXER_GAIN;
    s_airspy_config.airspy_mixer_gain_arg = AIRSPY_DEFAULT_MIXER_GAIN;
    s_airspy_config.vga_gain = AIRSPY_DEFAULT_VGA_GAIN;
    s_airspy_config.airspy_vga_gain_arg = AIRSPY_DEFAULT_VGA_GAIN;
}

static const struct argparse_option airspy_input_cli_options[] = {
    OPT_GROUP("Airspy Input (airspy)"),
    OPT_STRING(0, "airspy-gain-mode", &s_airspy_config.gain_mode, "Gain mode: 'linearity', 'sensitivity', or 'manual'. (Default: AGC)", NULL, 0, 0),
    OPT_INTEGER(0, "airspy-gain-value", &s_airspy_config.airspy_gain_value_arg, "Gain value for linearity/sensitivity modes (0-21). (Default: 10)", NULL, 0, 0),
    OPT_INTEGER(0, "airspy-lna-gain", &s_airspy_config.airspy_lna_gain_arg, "Manual LNA gain (0-14). Only with manual mode. (Default: 5)", NULL, 0, 0),
    OPT_INTEGER(0, "airspy-mixer-gain", &s_airspy_config.airspy_mixer_gain_arg, "Manual Mixer gain (0-15). Only with manual mode. (Default: 5)", NULL, 0, 0),
    OPT_INTEGER(0, "airspy-vga-gain", &s_airspy_config.airspy_vga_gain_arg, "Manual VGA gain (0-15). Only with manual mode. (Default: 5)", NULL, 0, 0),
    OPT_STRING(0, "airspy-sample-format", &s_airspy_config.sample_format, "Sample format: 'cf32' or 'cs16'. (Default: cs16)", NULL, 0, 0),
    OPT_INTEGER(0, "airspy-serial", (int*)&s_airspy_config.serial_number, "Select device by serial number (hex, e.g., 0x123456789ABCDEF0).", NULL, 0, 0),
    OPT_BOOLEAN(0, "airspy-packing", &s_airspy_config.packing_enabled, "Enable bit-packing mode (12-bit samples).", NULL, 0, 0),
};

const struct argparse_option* airspy_input_get_cli_options(int* count) {
    *count = sizeof(airspy_input_cli_options) / sizeof(airspy_input_cli_options[0]);
    return airspy_input_cli_options;
}

static bool airspy_input_initialize(ModuleContext* context);
static void* airspy_input_start_stream(ModuleContext* context, QueueSamples queue_samples, void* pipeline_context);
static void airspy_input_stop_stream(ModuleContext* context);
static void airspy_input_cleanup(ModuleContext* context);
static void airspy_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info);
static bool airspy_input_validate_options(AppConfig* config);
static bool airspy_input_validate_generic_options(const AppConfig* config);

static int airspy_input_buffered_stream_callback(airspy_transfer* transfer);


static InputModuleInterface s_airspy_input_api = {
    .initialize = airspy_input_initialize,
    .start_stream = airspy_input_start_stream,
    .stop_stream = airspy_input_stop_stream,
    .cleanup = airspy_input_cleanup,
    .get_summary_info = airspy_input_get_summary_info,
    .validate_options = airspy_input_validate_options,
    .validate_generic_options = airspy_input_validate_generic_options,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_airspy_get_module_api(void) {
    return &s_airspy_input_api;
}

static bool airspy_input_validate_generic_options(const AppConfig* config) {
    if (!config->sdr_general.rf_freq_provided) {
        log_error("Airspy input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool airspy_input_validate_options(AppConfig* config) {
    // Gain Mode Validation
    if (s_airspy_config.gain_mode) {
        s_airspy_config.gain_mode_provided = true;
        if (strcasecmp(s_airspy_config.gain_mode, "linearity") != 0 &&
            strcasecmp(s_airspy_config.gain_mode, "sensitivity") != 0 &&
            strcasecmp(s_airspy_config.gain_mode, "manual") != 0) {
            log_error("Invalid --airspy-gain-mode '%s'. Must be 'linearity', 'sensitivity', or 'manual'.", s_airspy_config.gain_mode);
            return false;
        }
    }

    // Gain Value Validation
    if (s_airspy_config.airspy_gain_value_arg != AIRSPY_DEFAULT_GAIN_VALUE) {
        if (s_airspy_config.airspy_gain_value_arg < 0 || s_airspy_config.airspy_gain_value_arg > 21) {
            log_error("Invalid --airspy-gain-value %d. Must be between 0 and 21.", s_airspy_config.airspy_gain_value_arg);
            return false;
        }
        s_airspy_config.gain_value = s_airspy_config.airspy_gain_value_arg;
        s_airspy_config.gain_value_provided = true;
    }

    // LNA Gain Validation
    if (s_airspy_config.airspy_lna_gain_arg != AIRSPY_DEFAULT_LNA_GAIN) {
        if (s_airspy_config.airspy_lna_gain_arg < 0 || s_airspy_config.airspy_lna_gain_arg > 14) {
            log_error("Invalid --airspy-lna-gain %d. Must be between 0 and 14.", s_airspy_config.airspy_lna_gain_arg);
            return false;
        }
        s_airspy_config.lna_gain = s_airspy_config.airspy_lna_gain_arg;
        s_airspy_config.lna_gain_provided = true;
    }

    // Mixer Gain Validation
    if (s_airspy_config.airspy_mixer_gain_arg != AIRSPY_DEFAULT_MIXER_GAIN) {
        if (s_airspy_config.airspy_mixer_gain_arg < 0 || s_airspy_config.airspy_mixer_gain_arg > 15) {
            log_error("Invalid --airspy-mixer-gain %d. Must be between 0 and 15.", s_airspy_config.airspy_mixer_gain_arg);
            return false;
        }
        s_airspy_config.mixer_gain = s_airspy_config.airspy_mixer_gain_arg;
        s_airspy_config.mixer_gain_provided = true;
    }

    // VGA Gain Validation
    if (s_airspy_config.airspy_vga_gain_arg != AIRSPY_DEFAULT_VGA_GAIN) {
        if (s_airspy_config.airspy_vga_gain_arg < 0 || s_airspy_config.airspy_vga_gain_arg > 15) {
            log_error("Invalid --airspy-vga-gain %d. Must be between 0 and 15.", s_airspy_config.airspy_vga_gain_arg);
            return false;
        }
        s_airspy_config.vga_gain = s_airspy_config.airspy_vga_gain_arg;
        s_airspy_config.vga_gain_provided = true;
    }

    // Sample Format Validation
    if (s_airspy_config.sample_format) {
        s_airspy_config.sample_format_provided = true;
        if (strcasecmp(s_airspy_config.sample_format, "cf32") != 0 &&
            strcasecmp(s_airspy_config.sample_format, "cs16") != 0) {
            log_error("Invalid --airspy-sample-format '%s'. Must be 'cf32' or 'cs16'.", s_airspy_config.sample_format);
            return false;
        }
    }

    // Serial Number Validation
    if (s_airspy_config.serial_number != 0) {
        s_airspy_config.serial_provided = true;
    }

    // Sample Rate Validation
    if (config->sdr_general.sample_rate_provided) {
        static const uint32_t VALID_AIRSPY_RATES[] = {
            2500000,  // R2
            3000000,  // Mini
            6000000,  // Mini
            10000000  // R2 & Mini
        };
        const size_t NUM_RATES = sizeof(VALID_AIRSPY_RATES) / sizeof(VALID_AIRSPY_RATES[0]);

        bool rate_found = false;
        uint32_t requested = (uint32_t)config->sdr_general.sample_rate_hz;

        for (size_t i = 0; i < NUM_RATES; i++) {
            if (requested == VALID_AIRSPY_RATES[i]) {
                rate_found = true;
                break;
            }
        }

        if (!rate_found) {
            log_error("Invalid Airspy sample rate %u Hz.", requested);
            log_error("Valid sample rates for Airspy Mini: 3000000, 6000000, 10000000");
            log_error("Valid sample rates for Airspy R2: 2500000, 10000000");
            return false;
        }
    }

    // Validate that manual gain options are only used with manual mode
    if ((s_airspy_config.lna_gain_provided || s_airspy_config.mixer_gain_provided || s_airspy_config.vga_gain_provided) &&
        s_airspy_config.gain_mode_provided && strcasecmp(s_airspy_config.gain_mode, "manual") != 0) {
        log_error("Manual gain options (--airspy-lna-gain, --airspy-mixer-gain, --airspy-vga-gain) can only be used with --airspy-gain-mode manual.");
        return false;
    }

    // Validate that gain-value is only used with linearity/sensitivity modes
    if (s_airspy_config.gain_value_provided && s_airspy_config.gain_mode_provided &&
        strcasecmp(s_airspy_config.gain_mode, "manual") == 0) {
        log_error("Option --airspy-gain-value cannot be used with --airspy-gain-mode manual. Use --airspy-lna-gain, --airspy-mixer-gain, and --airspy-vga-gain instead.");
        return false;
    }

    return true;
}

static int airspy_input_buffered_stream_callback(airspy_transfer* transfer) {
    AppContext* app = (AppContext*)transfer->ctx;

    // --- HEARTBEAT ---

    if (is_shutdown_requested() || app->stats.error_occurred) {
        return -1;
    }

    if (transfer->sample_count == 0) {
        return 0;
    }

    // Determine the sample format from the transfer
    switch (transfer->sample_type) {
        case AIRSPY_SAMPLE_INT16_IQ:
            // When packing is enabled, the library unpacks to INT16_IQ automatically
            if (!app->module.queue_samples(app->module.pipeline_context, transfer->samples, transfer->sample_count, CS16)) {
        /* Warning handled internally by pipeline */
    }
            break;

        case AIRSPY_SAMPLE_FLOAT32_IQ:
            if (!app->module.queue_samples(app->module.pipeline_context, transfer->samples, transfer->sample_count, CF32)) {
        /* Warning handled internally by pipeline */
    }
            break;

        case AIRSPY_SAMPLE_RAW:
            // We force INT16_IQ mode during initialization, so we should not see RAW here.
            // If we do, it implies the library failed to unpack or configuration is wrong.
            handle_fatal_thread_error("Airspy unexpected RAW sample type. Library unpacking config error?", app);
            return -1;

        case AIRSPY_SAMPLE_INT16_REAL:
        case AIRSPY_SAMPLE_FLOAT32_REAL:
        case AIRSPY_SAMPLE_UINT16_REAL:
            // Real (non-IQ) sample formats are not supported by our pipeline
            handle_fatal_thread_error("Airspy real-only sample format not supported. Pipeline requires I/Q samples.", app);
            return -1;
            
        default:
            handle_fatal_thread_error("Airspy unknown sample type received.", app);
            return -1;
    }

    return 0;
}


static void airspy_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info) {
    const AppConfig *config = context->config;
    const AppContext* app = context->app;
    AirspyContext* private_data = (AirspyContext*)app->module.input_private_data;

    // Use dynamic board name if available, else fallback
    const char* source_name = (private_data && private_data->board_name) ? private_data->board_name : "Airspy (Unknown)";
    add_summary_item(info, "Input Source", "%s", source_name);

    // Determine format string
    const char* format_str = "Unknown";
    if (private_data && private_data->sample_type == AIRSPY_SAMPLE_INT16_IQ) {
        format_str = "16-bit Signed Complex (cs16)";
    } else if (private_data && private_data->sample_type == AIRSPY_SAMPLE_FLOAT32_IQ) {
        format_str = "32-bit Float Complex (cf32)";
    }

    add_summary_item(info, "Input Format", "%s", format_str);
    add_summary_item(info, "Input Sample Rate", "%.15g Hz", (double)app->module.source_info.sample_rate);

    // Gain reporting
    if (s_airspy_config.gain_mode_provided) {
        if (strcasecmp(s_airspy_config.gain_mode, "linearity") == 0) {
            add_summary_item(info, "Gain Mode", "Linearity (Level: %d)", s_airspy_config.gain_value);
        } else if (strcasecmp(s_airspy_config.gain_mode, "sensitivity") == 0) {
            add_summary_item(info, "Gain Mode", "Sensitivity (Level: %d)", s_airspy_config.gain_value);
        } else if (strcasecmp(s_airspy_config.gain_mode, "manual") == 0) {
            add_summary_item(info, "Gain Mode", "Manual");
            add_summary_item(info, "LNA Gain", "%d", s_airspy_config.lna_gain);
            add_summary_item(info, "Mixer Gain", "%d", s_airspy_config.mixer_gain);
            add_summary_item(info, "VGA Gain", "%d", s_airspy_config.vga_gain);
        }
    } else {
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }

    if (s_airspy_config.packing_enabled) {
        add_summary_item(info, "Packing", "Enabled");
    }

    add_summary_item(info, "Bias-T", "%s", config->sdr_general.bias_t_enable ? "Enabled" : "Disabled");
}

static bool airspy_input_initialize(ModuleContext* context) {
    const AppConfig *config = context->config;
    AppContext* app = context->app;
    int result;
    bool success = false;

    AirspyContext* private_data = (AirspyContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(AirspyContext), true);
    if (!private_data) {
        return false;
    }
    private_data->dev = NULL;

    if (pthread_mutex_init(&private_data->driver_mutex, NULL) != 0) {
        log_error("Failed to init driver mutex.");
        return false;
    }
    private_data->board_name = "Airspy Unknown"; // Default

    app->module.input_private_data = private_data;

    result = airspy_init();
    if (result != AIRSPY_SUCCESS) {
        log_error("airspy_init() failed: %s (%d)", airspy_error_name(result), result);
        goto cleanup;
    }

    // Open device
    if (s_airspy_config.serial_provided) {
        result = airspy_open_sn(&private_data->dev, s_airspy_config.serial_number);
    } else {
        result = airspy_open(&private_data->dev);
    }

    if (result != AIRSPY_SUCCESS) {
        log_error("airspy_open() failed: %s (%d)", airspy_error_name(result), result);
        private_data->dev = NULL;
        goto cleanup;
    }

    // --- Identify Board ---
    // Note: Board ID logic relies on the compatibility defines at top of file
    uint8_t board_id = 0;
    result = airspy_board_id_read(private_data->dev, &board_id);
    if (result == AIRSPY_SUCCESS) {
        switch (board_id) {
            case AIRSPY_BOARD_ID_AIRSPY_R2:
                private_data->board_name = "Airspy R2";
                break;
            case AIRSPY_BOARD_ID_AIRSPY_MINI:
                private_data->board_name = "Airspy Mini";
                break;
            default:
                private_data->board_name = "Unknown Airspy";
                break;
        }
    }
    // Read 128-bit Serial Number (4 chunks of 32-bits)
    airspy_read_partid_serialno_t s = {0};
    airspy_board_partid_serialno_read(private_data->dev, &s);

    log_info("Using Airspy device: %s (S/N: 0x%08X%08X%08X%08X)",
             private_data->board_name,
             s.serial_no[0], s.serial_no[1], s.serial_no[2], s.serial_no[3]);

    // Get and validate supported sample rates
    uint32_t num_rates = 0;
    result = airspy_get_samplerates(private_data->dev, &num_rates, 0);
    if (result != AIRSPY_SUCCESS) {
        log_error("Failed to get number of supported sample rates: %s (%d)", airspy_error_name(result), result);
        goto cleanup;
    }

    uint32_t* rates = (uint32_t*)mem_arena_alloc(&app->pipeline.setup_arena, num_rates * sizeof(uint32_t), false);
    if (!rates) {
        goto cleanup;
    }

    result = airspy_get_samplerates(private_data->dev, rates, num_rates);
    if (result != AIRSPY_SUCCESS) {
        log_error("Failed to get supported sample rates: %s (%d)", airspy_error_name(result), result);
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
        log_error("Requested sample rate %.15g Hz is not supported by this Airspy device.", config->sdr_general.sample_rate_hz);
        log_error("Supported rates are:");
        for (uint32_t i = 0; i < num_rates; i++) {
            log_error("  %u Hz", rates[i]);
        }
        goto cleanup;
    }

    // Set sample rate
    result = airspy_set_samplerate(private_data->dev, requested_rate);
    if (result != AIRSPY_SUCCESS) {
        log_error("airspy_set_samplerate() failed: %s (%d)", airspy_error_name(result), result);
        goto cleanup;
    }

    // Set frequency
    result = airspy_set_freq(private_data->dev, (uint32_t)config->sdr_general.rf_freq_hz);
    if (result != AIRSPY_SUCCESS) {
        log_error("airspy_set_freq() failed: %s (%d)", airspy_error_name(result), result);
        goto cleanup;
    }

    // Determine sample format and packing
    // Default assumption
    private_data->sample_type = AIRSPY_SAMPLE_INT16_IQ;
    app->module.input_format = CS16;

    if (s_airspy_config.packing_enabled) {
        log_info("Enabling bit-packing mode (12-bit).");
        result = airspy_set_packing(private_data->dev, 1);
        if (result != AIRSPY_SUCCESS) {
            log_warn("Failed to enable packing mode: %s (%d)", airspy_error_name(result), result);
        } else {
            // FORCE format to CS16 if packing enabled, even if user asked for CF32.
            if (s_airspy_config.sample_format_provided && 
                strcasecmp(s_airspy_config.sample_format, "cf32") == 0) {
                log_warn("Airspy Packing enabled: Overriding sample format to CS16 (Packed mode does not support Float).");
            }
            app->module.input_format = CS16;
            
            // When packing is enabled, we MUST force the sample type to INT16_IQ.
            // This tells the library to unpack the raw 12-bit data into 16-bit integers for us.
            private_data->sample_type = AIRSPY_SAMPLE_INT16_IQ;
        }
    } else if (s_airspy_config.sample_format_provided) {
        if (strcasecmp(s_airspy_config.sample_format, "cf32") == 0) {
            private_data->sample_type = AIRSPY_SAMPLE_FLOAT32_IQ;
            app->module.input_format = CF32;
        }
    }

    // Set sample type
    // This needs to happen regardless of packing status to ensure the library knows
    // what output format we expect (INT16_IQ or FLOAT32_IQ).
    result = airspy_set_sample_type(private_data->dev, private_data->sample_type);
    if (result != AIRSPY_SUCCESS) {
        log_error("airspy_set_sample_type() failed: %s (%d)", airspy_error_name(result), result);
        goto cleanup;
    }

    // Configure gain
    bool any_gain_option_specified = s_airspy_config.gain_mode_provided ||
                                      s_airspy_config.lna_gain_provided ||
                                      s_airspy_config.mixer_gain_provided ||
                                      s_airspy_config.vga_gain_provided;

    if (!any_gain_option_specified) {
        // Enable AGC
        log_info("Enabling automatic gain control (AGC)...");
        result = airspy_set_lna_agc(private_data->dev, 1);
        if (result != AIRSPY_SUCCESS) {
            log_error("airspy_set_lna_agc() failed: %s (%d)", airspy_error_name(result), result);
            goto cleanup;
        }
        result = airspy_set_mixer_agc(private_data->dev, 1);
        if (result != AIRSPY_SUCCESS) {
            log_error("airspy_set_mixer_agc() failed: %s (%d)", airspy_error_name(result), result);
            goto cleanup;
        }
    } else {
        // Disable AGC
        result = airspy_set_lna_agc(private_data->dev, 0);
        if (result != AIRSPY_SUCCESS) {
            log_error("airspy_set_lna_agc() failed: %s (%d)", airspy_error_name(result), result);
            goto cleanup;
        }
        result = airspy_set_mixer_agc(private_data->dev, 0);
        if (result != AIRSPY_SUCCESS) {
            log_error("airspy_set_mixer_agc() failed: %s (%d)", airspy_error_name(result), result);
            goto cleanup;
        }

        // Apply gain settings
        if (s_airspy_config.gain_mode_provided) {
            if (strcasecmp(s_airspy_config.gain_mode, "linearity") == 0) {
                result = airspy_set_linearity_gain(private_data->dev, s_airspy_config.gain_value);
                if (result != AIRSPY_SUCCESS) {
                    log_error("airspy_set_linearity_gain() failed: %s (%d)", airspy_error_name(result), result);
                    goto cleanup;
                }
            } else if (strcasecmp(s_airspy_config.gain_mode, "sensitivity") == 0) {
                result = airspy_set_sensitivity_gain(private_data->dev, s_airspy_config.gain_value);
                if (result != AIRSPY_SUCCESS) {
                    log_error("airspy_set_sensitivity_gain() failed: %s (%d)", airspy_error_name(result), result);
                    goto cleanup;
                }
            } else if (strcasecmp(s_airspy_config.gain_mode, "manual") == 0) {
                result = airspy_set_lna_gain(private_data->dev, s_airspy_config.lna_gain);
                if (result != AIRSPY_SUCCESS) {
                    log_error("airspy_set_lna_gain() failed: %s (%d)", airspy_error_name(result), result);
                    goto cleanup;
                }
                result = airspy_set_mixer_gain(private_data->dev, s_airspy_config.mixer_gain);
                if (result != AIRSPY_SUCCESS) {
                    log_error("airspy_set_mixer_gain() failed: %s (%d)", airspy_error_name(result), result);
                    goto cleanup;
                }
                result = airspy_set_vga_gain(private_data->dev, s_airspy_config.vga_gain);
                if (result != AIRSPY_SUCCESS) {
                    log_error("airspy_set_vga_gain() failed: %s (%d)", airspy_error_name(result), result);
                    goto cleanup;
                }
            }
        }
    }

    // Set Bias-T if requested
    if (config->sdr_general.bias_t_enable) {
        log_info("Enabling Bias-T...");
        result = airspy_set_rf_bias(private_data->dev, 1);
        if (result != AIRSPY_SUCCESS) {
            log_error("airspy_set_rf_bias() failed: %s (%d)", airspy_error_name(result), result);
            goto cleanup;
        }
    }

    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(app->module.input_format);
    app->module.source_info.sample_rate = (int)config->sdr_general.sample_rate_hz;
    app->module.source_info.frames = -1;


    success = true;

cleanup:
    if (!success) {
        // Cleanup will be handled by airspy_input_cleanup()
    }
    return success;
}

static void* airspy_input_start_stream(ModuleContext* context, QueueSamples queue_samples, void* pipeline_context) {
    context->app->module.queue_samples = queue_samples;
    context->app->module.pipeline_context = pipeline_context;
    AppContext* app = context->app;
    AirspyContext* private_data = (AirspyContext*)app->module.input_private_data;
    int result;
    airspy_sample_block_cb_fn callback_fn;
    callback_fn = airspy_input_buffered_stream_callback;

    result = airspy_start_rx(private_data->dev, callback_fn, app);
    if (result != AIRSPY_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "airspy_start_rx() failed: %s (%d)", airspy_error_name(result), result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    // Wait for the shutdown signal (Event-driven)
    if (app->pipeline.shutdown_event) {
        wait_event_wait(app->pipeline.shutdown_event);
    }

    if (!is_shutdown_requested()) {
        airspy_input_stop_stream(context);
    }

    return NULL;
}

static void airspy_input_stop_stream(ModuleContext* context) {
    AppContext* app = context->app;
    AirspyContext* private_data = (AirspyContext*)app->module.input_private_data;
    if (private_data) {
    pthread_mutex_lock(&private_data->driver_mutex);
    if (private_data && private_data->dev && airspy_is_streaming(private_data->dev) == AIRSPY_TRUE) {
        log_debug("Stopping Airspy stream...");
        int result = airspy_stop_rx(private_data->dev);
        if (result != AIRSPY_SUCCESS) {
            log_error("Failed to stop Airspy RX: %s (%d)", airspy_error_name(result), result);
        }
    }
    pthread_mutex_unlock(&private_data->driver_mutex);
}
}

static void airspy_input_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (app->module.input_private_data) {
        AirspyContext* private_data = (AirspyContext*)app->module.input_private_data;
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->dev) {
            airspy_close(private_data->dev);
            private_data->dev = NULL;
        }
        pthread_mutex_unlock(&private_data->driver_mutex);
        pthread_mutex_destroy(&private_data->driver_mutex);
        app->module.input_private_data = NULL;
    }
    log_debug("Exiting Airspy library...");
    airspy_exit();
}
