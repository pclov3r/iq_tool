/**
 * @file input_hydrasdr.c
 */

#include "input_hydrasdr.h"
#include "module.h"
#include "constants.h"
#include "module_defaults.h"
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
#include <hydrasdr.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#include <strings.h>
#endif

extern pthread_mutex_t g_console_mutex;

// Max expected buffer size for unpacking.
// HydraSDR USB transfers are typically 128KB - 256KB.
// We'll allocate enough space for 256K complex samples (16-bit I/Q) to be safe.
// 262144 samples * 4 bytes/sample = ~1MB.
#define HYDRASDR_UNPACK_BUFFER_SAMPLES 262144

// --- Private Module Configuration ---
static struct {
    char* gain_mode;
    bool gain_mode_provided;
    int gain_value;
    bool gain_value_provided;
    int hydrasdr_gain_value_arg;
    int lna_gain;
    bool lna_gain_provided;
    int hydrasdr_lna_gain_arg;
    int mixer_gain;
    bool mixer_gain_provided;
    int hydrasdr_mixer_gain_arg;
    int vga_gain;
    bool vga_gain_provided;
    int hydrasdr_vga_gain_arg;
    char* sample_format;
    bool sample_format_provided;
    uint64_t serial_number;
    bool serial_provided;
    bool packing_enabled;
} s_hydrasdr_config;

// --- Private Module State ---
typedef struct {
    struct hydrasdr_device* dev;
    enum hydrasdr_sample_type sample_type;
    const char* board_name;         // "HydraSDR R2", "HydraSDR Mini", etc.
    pthread_mutex_t driver_mutex;
} HydraSDRContext;

void hydrasdr_set_default_config(AppConfig* config) {
    config->sdr_general.sample_rate_hz = HYDRASDR_DEFAULT_SAMPLE_RATE;
    s_hydrasdr_config.gain_value = HYDRASDR_DEFAULT_GAIN_VALUE;
    s_hydrasdr_config.hydrasdr_gain_value_arg = HYDRASDR_DEFAULT_GAIN_VALUE;
    s_hydrasdr_config.lna_gain = HYDRASDR_DEFAULT_LNA_GAIN;
    s_hydrasdr_config.hydrasdr_lna_gain_arg = HYDRASDR_DEFAULT_LNA_GAIN;
    s_hydrasdr_config.mixer_gain = HYDRASDR_DEFAULT_MIXER_GAIN;
    s_hydrasdr_config.hydrasdr_mixer_gain_arg = HYDRASDR_DEFAULT_MIXER_GAIN;
    s_hydrasdr_config.vga_gain = HYDRASDR_DEFAULT_VGA_GAIN;
    s_hydrasdr_config.hydrasdr_vga_gain_arg = HYDRASDR_DEFAULT_VGA_GAIN;
}

static const struct argparse_option hydrasdr_input_cli_options[] = {
    OPT_GROUP("HydraSDR Input (hydrasdr)"),
    OPT_STRING(0, "hydrasdr-gain-mode", &s_hydrasdr_config.gain_mode, "Gain mode: 'linearity', 'sensitivity', or 'manual'. (Default: AGC)", NULL, 0, 0),
    OPT_INTEGER(0, "hydrasdr-gain-value", &s_hydrasdr_config.hydrasdr_gain_value_arg, "Gain value for linearity/sensitivity modes (0-21). (Default: 10)", NULL, 0, 0),
    OPT_INTEGER(0, "hydrasdr-lna-gain", &s_hydrasdr_config.hydrasdr_lna_gain_arg, "Manual LNA gain (0-14). Only with manual mode. (Default: 5)", NULL, 0, 0),
    OPT_INTEGER(0, "hydrasdr-mixer-gain", &s_hydrasdr_config.hydrasdr_mixer_gain_arg, "Manual Mixer gain (0-15). Only with manual mode. (Default: 5)", NULL, 0, 0),
    OPT_INTEGER(0, "hydrasdr-vga-gain", &s_hydrasdr_config.hydrasdr_vga_gain_arg, "Manual VGA gain (0-15). Only with manual mode. (Default: 5)", NULL, 0, 0),
    OPT_STRING(0, "hydrasdr-sample-format", &s_hydrasdr_config.sample_format, "Sample format: 'cf32' or 'cs16'. (Default: cs16)", NULL, 0, 0),
    OPT_INTEGER(0, "hydrasdr-serial", (int*)&s_hydrasdr_config.serial_number, "Select device by serial number (hex, e.g., 0x123456789ABCDEF0).", NULL, 0, 0),
    OPT_BOOLEAN(0, "hydrasdr-packing", &s_hydrasdr_config.packing_enabled, "Enable bit-packing mode (12-bit samples).", NULL, 0, 0),
};

const struct argparse_option* hydrasdr_input_get_cli_options(int* count) {
    *count = sizeof(hydrasdr_input_cli_options) / sizeof(hydrasdr_input_cli_options[0]);
    return hydrasdr_input_cli_options;
}

static void hydrasdr_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info);
static bool hydrasdr_input_validate_options(AppContext* app);
static bool hydrasdr_input_validate_generic_options(const AppConfig* config);

static int hydrasdr_input_buffered_stream_callback(hydrasdr_transfer* transfer);

static bool hydrasdr_input_validate_generic_options(const AppConfig* config) {
    if (!config->sdr_general.rf_freq_provided) {
        log_error("HydraSDR input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool hydrasdr_input_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    // Gain Mode Validation
    if (s_hydrasdr_config.gain_mode) {
        s_hydrasdr_config.gain_mode_provided = true;
        if (strcasecmp(s_hydrasdr_config.gain_mode, "linearity") != 0 &&
            strcasecmp(s_hydrasdr_config.gain_mode, "sensitivity") != 0 &&
            strcasecmp(s_hydrasdr_config.gain_mode, "manual") != 0) {
            log_error("Invalid --hydrasdr-gain-mode '%s'. Must be 'linearity', 'sensitivity', or 'manual'.", s_hydrasdr_config.gain_mode);
            return false;
        }
    }

    // Gain Value Validation
    if (s_hydrasdr_config.hydrasdr_gain_value_arg != HYDRASDR_DEFAULT_GAIN_VALUE) {
        if (s_hydrasdr_config.hydrasdr_gain_value_arg < 0 || s_hydrasdr_config.hydrasdr_gain_value_arg > 21) {
            log_error("Invalid --hydrasdr-gain-value %d. Must be between 0 and 21.", s_hydrasdr_config.hydrasdr_gain_value_arg);
            return false;
        }
        s_hydrasdr_config.gain_value = s_hydrasdr_config.hydrasdr_gain_value_arg;
        s_hydrasdr_config.gain_value_provided = true;
    }

    // LNA Gain Validation
    if (s_hydrasdr_config.hydrasdr_lna_gain_arg != HYDRASDR_DEFAULT_LNA_GAIN) {
        if (s_hydrasdr_config.hydrasdr_lna_gain_arg < 0 || s_hydrasdr_config.hydrasdr_lna_gain_arg > 14) {
            log_error("Invalid --hydrasdr-lna-gain %d. Must be between 0 and 14.", s_hydrasdr_config.hydrasdr_lna_gain_arg);
            return false;
        }
        s_hydrasdr_config.lna_gain = s_hydrasdr_config.hydrasdr_lna_gain_arg;
        s_hydrasdr_config.lna_gain_provided = true;
    }

    // Mixer Gain Validation
    if (s_hydrasdr_config.hydrasdr_mixer_gain_arg != HYDRASDR_DEFAULT_MIXER_GAIN) {
        if (s_hydrasdr_config.hydrasdr_mixer_gain_arg < 0 || s_hydrasdr_config.hydrasdr_mixer_gain_arg > 15) {
            log_error("Invalid --hydrasdr-mixer-gain %d. Must be between 0 and 15.", s_hydrasdr_config.hydrasdr_mixer_gain_arg);
            return false;
        }
        s_hydrasdr_config.mixer_gain = s_hydrasdr_config.hydrasdr_mixer_gain_arg;
        s_hydrasdr_config.mixer_gain_provided = true;
    }

    // VGA Gain Validation
    if (s_hydrasdr_config.hydrasdr_vga_gain_arg != HYDRASDR_DEFAULT_VGA_GAIN) {
        if (s_hydrasdr_config.hydrasdr_vga_gain_arg < 0 || s_hydrasdr_config.hydrasdr_vga_gain_arg > 15) {
            log_error("Invalid --hydrasdr-vga-gain %d. Must be between 0 and 15.", s_hydrasdr_config.hydrasdr_vga_gain_arg);
            return false;
        }
        s_hydrasdr_config.vga_gain = s_hydrasdr_config.hydrasdr_vga_gain_arg;
        s_hydrasdr_config.vga_gain_provided = true;
    }

    // Sample Format Validation
    if (s_hydrasdr_config.sample_format) {
        s_hydrasdr_config.sample_format_provided = true;
        if (strcasecmp(s_hydrasdr_config.sample_format, "cf32") != 0 &&
            strcasecmp(s_hydrasdr_config.sample_format, "cs16") != 0 &&
            strcasecmp(s_hydrasdr_config.sample_format, "f32") != 0 &&
            strcasecmp(s_hydrasdr_config.sample_format, "s16") != 0 &&
            strcasecmp(s_hydrasdr_config.sample_format, "u16") != 0) {
            log_error("Invalid --hydrasdr-sample-format '%s'. Must be 'cf32', 'cs16', 'f32', 's16', or 'u16'.", s_hydrasdr_config.sample_format);
            return false;
        }
    }

    // Serial Number Validation
    if (s_hydrasdr_config.serial_number != 0) {
        s_hydrasdr_config.serial_provided = true;
    }


    // Sample Rate Validation
    if (config->sdr_general.sample_rate_provided) {
        // We defer sample rate validation to the init function where we can query
        // the device for its dynamically supported decimation rates.
    }

    // Validate that manual gain options are only used with manual mode
    if ((s_hydrasdr_config.lna_gain_provided || s_hydrasdr_config.mixer_gain_provided || s_hydrasdr_config.vga_gain_provided) &&
        s_hydrasdr_config.gain_mode_provided && strcasecmp(s_hydrasdr_config.gain_mode, "manual") != 0) {
        log_error("Manual gain options (--hydrasdr-lna-gain, --hydrasdr-mixer-gain, --hydrasdr-vga-gain) can only be used with --hydrasdr-gain-mode manual.");
        return false;
    }

    // Validate that gain-value is only used with linearity/sensitivity modes
    if (s_hydrasdr_config.gain_value_provided && s_hydrasdr_config.gain_mode_provided &&
        strcasecmp(s_hydrasdr_config.gain_mode, "manual") == 0) {
        log_error("Option --hydrasdr-gain-value cannot be used with --hydrasdr-gain-mode manual. Use --hydrasdr-lna-gain, --hydrasdr-mixer-gain, and --hydrasdr-vga-gain instead.");
        return false;
    }

    return true;
}

static int hydrasdr_input_buffered_stream_callback(hydrasdr_transfer* transfer) {
    AppContext* app = (AppContext*)transfer->ctx;

    if (is_shutdown_requested() || app->stats.error_occurred) {
        return -1;
    }

    if (transfer->sample_count == 0) {
        return 0;
    }

    // Determine the sample format from the transfer
    switch (transfer->sample_type) {
        case HYDRASDR_SAMPLE_INT16_IQ:
            // When packing is enabled, the library unpacks to INT16_IQ automatically
            if (!app->module.queue_samples(app->module.pipeline_context, transfer->samples, transfer->sample_count, CS16)) {}
            break;

        case HYDRASDR_SAMPLE_FLOAT32_IQ:
            if (!app->module.queue_samples(app->module.pipeline_context, transfer->samples, transfer->sample_count, CF32)) {}
            break;

        case HYDRASDR_SAMPLE_FLOAT32_REAL:
            if (!app->module.queue_samples(app->module.pipeline_context, transfer->samples, transfer->sample_count, F32)) {}
            break;

        case HYDRASDR_SAMPLE_INT16_REAL:
            if (!app->module.queue_samples(app->module.pipeline_context, transfer->samples, transfer->sample_count, S16)) {}
            break;

        case HYDRASDR_SAMPLE_UINT16_REAL:
            if (!app->module.queue_samples(app->module.pipeline_context, transfer->samples, transfer->sample_count, U16)) {}
            break;

        case HYDRASDR_SAMPLE_RAW:
            // We force INT16_IQ mode during initialization, so we should not see RAW here.
            // If we do, it implies the library failed to unpack or configuration is wrong.
            handle_fatal_thread_error("HydraSDR unexpected RAW sample type. Library unpacking config error?", app);
            return -1;

        default:
            handle_fatal_thread_error("HydraSDR unknown sample type received.", app);
            return -1;
    }

    return 0;
}

static void hydrasdr_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info) {
    const AppConfig *config = context->config;
    const AppContext* app = context->app;
    HydraSDRContext* private_data = (HydraSDRContext*)app->module.input_private_data;

    // Use dynamic board name if available, else fallback
    const char* source_name = (private_data && private_data->board_name) ? private_data->board_name : "HydraSDR (Unknown)";
    add_summary_item(info, "Input Source", "%s", source_name);

    // Determine format string
    const char* format_str = "Unknown";
    if (private_data && private_data->sample_type == HYDRASDR_SAMPLE_INT16_IQ) {
        format_str = "16-bit Signed Complex (cs16)";
    } else if (private_data && private_data->sample_type == HYDRASDR_SAMPLE_FLOAT32_IQ) {
        format_str = "32-bit Float Complex (cf32)";
    }

    add_summary_item(info, "Input Format", "%s", format_str);
    add_summary_item(info, "Input Sample Rate", "%.15g Hz", (double)app->module.source_info.sample_rate);

    // Gain reporting
    if (s_hydrasdr_config.gain_mode_provided) {
        if (strcasecmp(s_hydrasdr_config.gain_mode, "linearity") == 0) {
            add_summary_item(info, "Gain Mode", "Linearity (Level: %d)", s_hydrasdr_config.gain_value);
        } else if (strcasecmp(s_hydrasdr_config.gain_mode, "sensitivity") == 0) {
            add_summary_item(info, "Gain Mode", "Sensitivity (Level: %d)", s_hydrasdr_config.gain_value);
        } else if (strcasecmp(s_hydrasdr_config.gain_mode, "manual") == 0) {
            add_summary_item(info, "Gain Mode", "Manual");
            add_summary_item(info, "LNA Gain", "%d", s_hydrasdr_config.lna_gain);
            add_summary_item(info, "Mixer Gain", "%d", s_hydrasdr_config.mixer_gain);
            add_summary_item(info, "VGA Gain", "%d", s_hydrasdr_config.vga_gain);
        }
    } else {
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }

    if (s_hydrasdr_config.packing_enabled) {
        add_summary_item(info, "Packing", "Enabled");
    }

    add_summary_item(info, "Bias-T", "%s", config->sdr_general.bias_t_enable ? "Enabled" : "Disabled");
}

static bool hydrasdr_input_initialize(ModuleContext* context) {
    const AppConfig *config = context->config;
    AppContext* app = context->app;
    int result;
    bool success = false;

    HydraSDRContext* private_data = (HydraSDRContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(HydraSDRContext), true);
    if (!private_data) {
        return false;
    }
    private_data->dev = NULL;

    if (pthread_mutex_init(&private_data->driver_mutex, NULL) != 0) {
        log_error("Failed to init driver mutex.");
        return false;
    }
    private_data->board_name = "HydraSDR Unknown"; // Default

    app->module.input_private_data = private_data;

    // Open device
    if (s_hydrasdr_config.serial_provided) {
        result = hydrasdr_open_sn(&private_data->dev, s_hydrasdr_config.serial_number);
    } else {
        result = hydrasdr_open(&private_data->dev);
    }

    if (result != HYDRASDR_SUCCESS) {
        log_error("hydrasdr_open() failed: %s (%d)", hydrasdr_error_name(result), result);
        private_data->dev = NULL;
        goto cleanup;
    }

    // --- STEP 1: Capability Query & Identify Board ---
    hydrasdr_device_info_t info = {0};
    result = hydrasdr_get_device_info(private_data->dev, &info);
    if (result == HYDRASDR_SUCCESS) {
        size_t name_len = strlen(info.board_name);
        char* safe_name = (char*)mem_arena_alloc(&app->pipeline.setup_arena, name_len + 1, false);
        strcpy(safe_name, info.board_name);
        private_data->board_name = safe_name;
    } else {
        private_data->board_name = "Unknown HydraSDR";
    }
    log_info("Using HydraSDR device: %s", private_data->board_name);

    // Get and validate supported sample rates
    uint32_t num_rates = 0;
    result = hydrasdr_get_samplerates(private_data->dev, &num_rates, 0);
    if (result != HYDRASDR_SUCCESS) {
        log_error("Failed to get number of supported sample rates: %s (%d)", hydrasdr_error_name(result), result);
        goto cleanup;
    }

    uint32_t* rates = (uint32_t*)malloc(num_rates * sizeof(uint32_t));
    if (!rates) {
        goto cleanup;
    }

    result = hydrasdr_get_samplerates(private_data->dev, rates, num_rates);
    if (result != HYDRASDR_SUCCESS) {
        free(rates);
        log_error("Failed to get supported sample rates: %s (%d)", hydrasdr_error_name(result), result);
        goto cleanup;
    }

    bool rate_supported = false;
    uint32_t requested_rate = (uint32_t)config->sdr_general.sample_rate_hz;
    for (uint32_t i = 0; i < num_rates; i++) {
        if (rates[i] == requested_rate) {
            rate_supported = true;
            break;
        }
    }

    if (!rate_supported) {
        log_error("Requested sample rate %.15g Hz is not supported.", config->sdr_general.sample_rate_hz);
        log_error("Supported rates are:");
        for (uint32_t i = 0; i < num_rates; i++) {
            log_error("  %u Hz", rates[i]);
        }
        free(rates);
        goto cleanup;
    }
    free(rates);

    // Determine sample format and packing
    // Default assumption
    private_data->sample_type = HYDRASDR_SAMPLE_INT16_IQ;
    app->module.input_format = CS16;

    if (s_hydrasdr_config.packing_enabled) {
        log_info("Enabling bit-packing mode (12-bit).");
        result = hydrasdr_set_packing(private_data->dev, 1);
        if (result != HYDRASDR_SUCCESS) {
            log_warn("Failed to enable packing mode: %s (%d)", hydrasdr_error_name(result), result);
        } else {
            // FORCE format to CS16 if packing enabled, even if user asked for CF32.
            if (s_hydrasdr_config.sample_format_provided &&
                strcasecmp(s_hydrasdr_config.sample_format, "cf32") == 0) {
                log_warn("HydraSDR Packing enabled: Overriding sample format to CS16 (Packed mode does not support Float).");
            }
            app->module.input_format = CS16;

            // When packing is enabled, we MUST force the sample type to INT16_IQ.
            // This tells the library to unpack the raw 12-bit data into 16-bit integers for us.
            private_data->sample_type = HYDRASDR_SAMPLE_INT16_IQ;
        }
    } else if (s_hydrasdr_config.sample_format_provided) {
        // --- Complex (IQ) Formats ---
        if (strcasecmp(s_hydrasdr_config.sample_format, "cf32") == 0) {
            private_data->sample_type = HYDRASDR_SAMPLE_FLOAT32_IQ;
            app->module.input_format = CF32;
        } else if (strcasecmp(s_hydrasdr_config.sample_format, "cs16") == 0) {
            private_data->sample_type = HYDRASDR_SAMPLE_INT16_IQ;
            app->module.input_format = CS16;
        }
        // --- Real (IF) Formats ---
        else if (strcasecmp(s_hydrasdr_config.sample_format, "f32") == 0) {
            private_data->sample_type = HYDRASDR_SAMPLE_FLOAT32_REAL;
            app->module.input_format = F32;
        } else if (strcasecmp(s_hydrasdr_config.sample_format, "s16") == 0) {
            private_data->sample_type = HYDRASDR_SAMPLE_INT16_REAL;
            app->module.input_format = S16;
        } else if (strcasecmp(s_hydrasdr_config.sample_format, "u16") == 0) {
            private_data->sample_type = HYDRASDR_SAMPLE_UINT16_REAL;
            app->module.input_format = U16;
        }
    }

    // Set Bias-T if requested
    if (config->sdr_general.bias_t_enable) {
        log_info("Enabling Bias-T...");
        result = hydrasdr_set_rf_bias(private_data->dev, 1);
        if (result != HYDRASDR_SUCCESS) {
            log_error("hydrasdr_set_rf_bias() failed: %s (%d)", hydrasdr_error_name(result), result);
            goto cleanup;
        }
    }

    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(app->module.input_format);
    app->module.source_info.sample_rate = (int)config->sdr_general.sample_rate_hz;
    app->module.source_info.frames = -1;

    success = true;

cleanup:
    if (!success) {
        // Cleanup will be handled by hydrasdr_input_cleanup()
    }
    return success;
}

static void hydrasdr_input_stop_sample_queue_push(ModuleContext* context);

static void* hydrasdr_input_push_samples_to_queue(ModuleContext* context, QueueSamples queue_samples, void* pipeline_context) {
    context->app->module.queue_samples = queue_samples;
    context->app->module.pipeline_context = pipeline_context;
    AppContext* app = context->app;
    HydraSDRContext* private_data = (HydraSDRContext*)app->module.input_private_data;
    int result;

    // Initialization order matching libhydrasdr README
    hydrasdr_device_info_t info = {0};
    result = hydrasdr_get_device_info(private_data->dev, &info);
    if (result != HYDRASDR_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hydrasdr_get_device_info() failed: %s (%d)", hydrasdr_error_name(result), result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    uint32_t sr_count = 0;
    hydrasdr_get_samplerates(private_data->dev, &sr_count, 0);
    uint32_t* samplerates = (uint32_t*)malloc(sr_count * sizeof(uint32_t));
    if (samplerates) {
        hydrasdr_get_samplerates(private_data->dev, samplerates, sr_count);
        free(samplerates);
    }

    result = hydrasdr_set_freq(private_data->dev, (uint64_t)app->config->sdr_general.rf_freq_hz);
    if (result != HYDRASDR_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hydrasdr_set_freq() failed: %s (%d)", hydrasdr_error_name(result), result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    // TODO: We skip setting decimation for now so it defaults to LOW_BANDWIDTH.
    // In the future, we may want to expose a CLI option to toggle HD mode.
    // Skipped bandwidth (Auto-bandwidth selected by library)

    result = hydrasdr_set_samplerate(private_data->dev, (uint32_t)app->config->sdr_general.sample_rate_hz);
    if (result != HYDRASDR_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hydrasdr_set_samplerate() failed: %s (%d)", hydrasdr_error_name(result), result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    result = hydrasdr_set_sample_type(private_data->dev, private_data->sample_type);
    if (result != HYDRASDR_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hydrasdr_set_sample_type() failed: %s (%d)", hydrasdr_error_name(result), result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    bool any_gain_option_specified = s_hydrasdr_config.gain_mode_provided ||
                                      s_hydrasdr_config.lna_gain_provided ||
                                      s_hydrasdr_config.mixer_gain_provided ||
                                      s_hydrasdr_config.vga_gain_provided;

    if (!any_gain_option_specified) {
        // Enable AGC only if supported
        log_info("Enabling automatic gain control (AGC)...");
        if (info.features & HYDRASDR_CAP_LNA_AGC) {
            hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_LNA_AGC, 1);
        }
        if (info.features & HYDRASDR_CAP_MIXER_AGC) {
            hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_MIXER_AGC, 1);
        }
    } else {
        // Disable AGC if supported
        if (info.features & HYDRASDR_CAP_LNA_AGC) hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_LNA_AGC, 0);
        if (info.features & HYDRASDR_CAP_MIXER_AGC) hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_MIXER_AGC, 0);

        // Apply gain settings
        if (s_hydrasdr_config.gain_mode_provided) {
            if (strcasecmp(s_hydrasdr_config.gain_mode, "linearity") == 0) {
                if (info.features & HYDRASDR_CAP_LINEARITY_GAIN) {
                    hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_LINEARITY, s_hydrasdr_config.gain_value);
                } else {
                    handle_fatal_thread_error("Linearity gain mode is not supported by this hardware.", app);
                    return NULL;
                }
            } else if (strcasecmp(s_hydrasdr_config.gain_mode, "sensitivity") == 0) {
                if (info.features & HYDRASDR_CAP_SENSITIVITY_GAIN) {
                    hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_SENSITIVITY, s_hydrasdr_config.gain_value);
                } else {
                    handle_fatal_thread_error("Sensitivity gain mode is not supported by this hardware.", app);
                    return NULL;
                }
            } else if (strcasecmp(s_hydrasdr_config.gain_mode, "manual") == 0) {
                if (info.features & HYDRASDR_CAP_LNA_GAIN) hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_LNA, s_hydrasdr_config.lna_gain);
                if (info.features & HYDRASDR_CAP_MIXER_GAIN) hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_MIXER, s_hydrasdr_config.mixer_gain);
                if (info.features & HYDRASDR_CAP_VGA_GAIN) hydrasdr_set_gain(private_data->dev, HYDRASDR_GAIN_TYPE_VGA, s_hydrasdr_config.vga_gain);
            }
        }
    }

    hydrasdr_sample_block_cb_fn callback_fn = hydrasdr_input_buffered_stream_callback;
    result = hydrasdr_start_rx(private_data->dev, callback_fn, app);
    if (result != HYDRASDR_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hydrasdr_start_rx() failed: %s (%d)", hydrasdr_error_name(result), result);
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    // Wait for the shutdown signal (Event-driven)
    if (app->pipeline.shutdown_event) {
        wait_event_wait(app->pipeline.shutdown_event);
    }

    if (!is_shutdown_requested()) {
        hydrasdr_input_stop_sample_queue_push(context);
    }

    return NULL;
}

static void hydrasdr_input_stop_sample_queue_push(ModuleContext* context) {
    AppContext* app = context->app;
    HydraSDRContext* private_data = (HydraSDRContext*)app->module.input_private_data;
    if (private_data) {
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->dev && hydrasdr_is_streaming(private_data->dev) == HYDRASDR_TRUE) {
            log_debug("Stopping HydraSDR stream...");
            int result = hydrasdr_stop_rx(private_data->dev);
            if (result != HYDRASDR_SUCCESS) {
                log_error("Failed to stop HydraSDR RX: %s (%d)", hydrasdr_error_name(result), result);
            }
        }
        pthread_mutex_unlock(&private_data->driver_mutex);
    }
}

static void hydrasdr_input_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (app->module.input_private_data) {
        HydraSDRContext* private_data = (HydraSDRContext*)app->module.input_private_data;
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->dev) {
            hydrasdr_close(private_data->dev);
            private_data->dev = NULL;
        }
        pthread_mutex_unlock(&private_data->driver_mutex);
        pthread_mutex_destroy(&private_data->driver_mutex);
        app->module.input_private_data = NULL;
    }
    log_debug("Exiting HydraSDR library...");
    }

// --- The InputModuleInterface V-Table ---
static InputModuleInterface s_hydrasdr_input_api = {
    .initialize = hydrasdr_input_initialize,
    .push_samples_to_queue = hydrasdr_input_push_samples_to_queue,
    .stop_sample_queue_push = hydrasdr_input_stop_sample_queue_push,
    .cleanup = hydrasdr_input_cleanup,
    .get_summary_info = hydrasdr_input_get_summary_info,
    .validate_options = hydrasdr_input_validate_options,
    .validate_generic_options = hydrasdr_input_validate_generic_options,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_hydrasdr_get_module_api(void) {
    return &s_hydrasdr_input_api;
}
