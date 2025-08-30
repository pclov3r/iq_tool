#include "input_bladerf.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "app_context.h"
#include "memory_arena.h"
#include "utils.h"
#include "sample_convert.h"
#include "platform.h"
#include "input_common.h"
#include "queue.h"
#include "sdr_packet_serializer.h"
#include "argparse.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

// Module-specific includes
#include <libbladeRF.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#include <windows.h>
#include <shlwapi.h>
#include <pathcch.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <libgen.h>
#endif

#if defined(_WIN32) && defined(WITH_BLADERF)
// --- Private Windows Dynamic API Loading ---
typedef struct {
    HINSTANCE dll_handle;
    void (*log_set_verbosity)(bladerf_log_level);
    int (*open)(struct bladerf **, const char *);
    void (*close)(struct bladerf *);
    const char * (*get_board_name)(struct bladerf *);
    int (*get_serial_struct)(struct bladerf *, struct bladerf_serial *);
    int (*is_fpga_configured)(struct bladerf *);
    int (*get_fpga_size)(struct bladerf *, bladerf_fpga_size *);
    int (*load_fpga)(struct bladerf *, const char *);
    int (*set_sample_rate)(struct bladerf *, bladerf_channel, bladerf_sample_rate, bladerf_sample_rate *);
    int (*set_rational_sample_rate)(struct bladerf *, bladerf_channel, struct bladerf_rational_rate *, struct bladerf_rational_rate *);
    int (*enable_feature)(struct bladerf *, bladerf_feature, bool);
    int (*set_bandwidth)(struct bladerf *, bladerf_channel, bladerf_bandwidth, bladerf_bandwidth *);
    int (*set_frequency)(struct bladerf *, bladerf_channel, bladerf_frequency);
    int (*set_gain_mode)(struct bladerf *, bladerf_channel, bladerf_gain_mode);
    int (*set_gain)(struct bladerf *, bladerf_channel, int);
    int (*set_bias_tee)(struct bladerf *, bladerf_channel, bool);
    int (*sync_config)(struct bladerf *, bladerf_channel_layout, bladerf_format, unsigned int, unsigned int, unsigned int, unsigned int);
    int (*enable_module)(struct bladerf *, bladerf_module, bool);
    int (*sync_rx)(struct bladerf *, void *, unsigned int, struct bladerf_metadata *, unsigned int);
    const char * (*strerror)(int);
} BladerfApiFunctionPointers;

static BladerfApiFunctionPointers bladerf_api;

#define LOAD_BLADERF_FUNC(func_name) \
    do { \
        FARPROC proc = GetProcAddress(bladerf_api.dll_handle, "bladerf_" #func_name); \
        if (!proc) { \
            log_fatal("Failed to load BladeRF API function: %s", "bladerf_" #func_name); \
            FreeLibrary(bladerf_api.dll_handle); \
            bladerf_api.dll_handle = NULL; \
            return false; \
        } \
        memcpy(&bladerf_api.func_name, &proc, sizeof(bladerf_api.func_name)); \
    } while (0)


static bool bladerf_load_api(void) {
    if (bladerf_api.dll_handle) { return true; }
    log_debug("Attempting to load bladeRF.dll...");
    bladerf_api.dll_handle = LoadLibraryA("bladeRF.dll");
    if (!bladerf_api.dll_handle) {
        print_win_error("LoadLibraryA for bladeRF.dll", GetLastError());
        log_error("Please ensure the BladeRF driver/library is installed and its directory is in the system PATH.");
        return false;
    }
    log_debug("BladeRF DLL loaded successfully. Loading function pointers...");
    LOAD_BLADERF_FUNC(log_set_verbosity);
    LOAD_BLADERF_FUNC(open);
    LOAD_BLADERF_FUNC(close);
    LOAD_BLADERF_FUNC(get_board_name);
    LOAD_BLADERF_FUNC(get_serial_struct);
    LOAD_BLADERF_FUNC(is_fpga_configured);
    LOAD_BLADERF_FUNC(get_fpga_size);
    LOAD_BLADERF_FUNC(load_fpga);
    LOAD_BLADERF_FUNC(set_sample_rate);
    LOAD_BLADERF_FUNC(set_rational_sample_rate);
    LOAD_BLADERF_FUNC(enable_feature);
    LOAD_BLADERF_FUNC(set_bandwidth);
    LOAD_BLADERF_FUNC(set_frequency);
    LOAD_BLADERF_FUNC(set_gain_mode);
    LOAD_BLADERF_FUNC(set_gain);
    LOAD_BLADERF_FUNC(set_bias_tee);
    LOAD_BLADERF_FUNC(sync_config);
    LOAD_BLADERF_FUNC(enable_module);
    LOAD_BLADERF_FUNC(sync_rx);
    LOAD_BLADERF_FUNC(strerror);
    log_debug("All BladeRF API function pointers loaded.");
    return true;
}

static void bladerf_unload_api(void) {
    if (bladerf_api.dll_handle) {
        FreeLibrary(bladerf_api.dll_handle);
        bladerf_api.dll_handle = NULL;
        log_debug("BladeRF API DLL unloaded.");
    }
}

#define bladerf_log_set_verbosity   bladerf_api.log_set_verbosity
#define bladerf_open            bladerf_api.open
#define bladerf_close           bladerf_api.close
#define bladerf_get_board_name  bladerf_api.get_board_name
#define bladerf_get_serial_struct bladerf_api.get_serial_struct
#define bladerf_is_fpga_configured bladerf_api.is_fpga_configured
#define bladerf_get_fpga_size   bladerf_api.get_fpga_size
#define bladerf_load_fpga       bladerf_api.load_fpga
#define bladerf_set_sample_rate bladerf_api.set_sample_rate
#define bladerf_set_rational_sample_rate bladerf_api.set_rational_sample_rate
#define bladerf_enable_feature  bladerf_api.enable_feature
#define bladerf_set_bandwidth   bladerf_api.set_bandwidth
#define bladerf_set_frequency   bladerf_api.set_frequency
#define bladerf_set_gain_mode   bladerf_api.set_gain_mode
#define bladerf_set_gain        bladerf_api.set_gain
#define bladerf_set_bias_tee    bladerf_api.set_bias_tee
#define bladerf_sync_config     bladerf_api.sync_config
#define bladerf_enable_module   bladerf_api.enable_module
#define bladerf_sync_rx         bladerf_api.sync_rx
#define bladerf_strerror        bladerf_api.strerror

#endif

extern AppConfig g_config;

// --- Private Module Configuration ---
static struct {
    int device_index;
    int channel;
    int gain;
    bool gain_provided;
    long bladerf_gain_arg;
    uint32_t bandwidth_hz;
    bool bandwidth_provided;
    float bladerf_bandwidth_hz_arg;
    char *fpga_file_path;
    unsigned int num_buffers;
    unsigned int buffer_size;
    unsigned int num_transfers;
    int bit_depth_arg;
    bool bit_depth_provided;
    int active_bit_depth;
} s_bladerf_config;

// --- Private Module State ---
typedef struct {
    struct bladerf *dev;
    char board_name[16];
    char serial[33];
    char display_name[128];
    void* stream_temp_buffer;
} BladerfPrivateData;


void bladerf_set_default_config(AppConfig* config) {
    config->sdr.sample_rate_hz = BLADERF_DEFAULT_SAMPLE_RATE_HZ;
    s_bladerf_config.bandwidth_hz = BLADERF_DEFAULT_BANDWIDTH_HZ;
}

static const struct argparse_option bladerf_cli_options[] = {
    OPT_GROUP("BladeRF-Specific Options"),
    OPT_INTEGER(0, "bladerf-device-idx", &s_bladerf_config.device_index, "Select specific BladeRF device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_STRING(0, "bladerf-load-fpga", &s_bladerf_config.fpga_file_path, "Load an FPGA bitstream from the specified file.", NULL, 0, 0),
    OPT_FLOAT(0, "bladerf-bandwidth", &s_bladerf_config.bladerf_bandwidth_hz_arg, "Set analog bandwidth in Hz. (Not applicable in 8-bit high-speed mode)", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-gain", &s_bladerf_config.bladerf_gain_arg, "Set overall manual gain in dB. Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-channel", &s_bladerf_config.channel, "For BladeRF 2.0: Select RX channel 0 (RXA) or 1 (RXB). (Default: 0)", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-bit-depth", &s_bladerf_config.bit_depth_arg, "Set capture bit depth {8|12}. 8-bit mode is for BladeRF 2.0 only. (Default: 12, auto-switches to 8 for rates > 61.44 MHz on BladeRF 2.0)", NULL, 0, 0),
};

const struct argparse_option* bladerf_get_cli_options(int* count) {
    *count = sizeof(bladerf_cli_options) / sizeof(bladerf_cli_options[0]);
    return bladerf_cli_options;
}

// Forward declarations for static functions
static bool bladerf_initialize(InputSourceContext* ctx);
static void* bladerf_start_stream(InputSourceContext* ctx);
static void bladerf_stop_stream(InputSourceContext* ctx);
static void bladerf_cleanup(InputSourceContext* ctx);
static void bladerf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool bladerf_validate_options(AppConfig* config);
static bool bladerf_validate_generic_options(const AppConfig* config);
static bool bladerf_find_and_load_fpga_automatically(struct bladerf* dev);
static bool bladerf_configure_standard_rate_and_rf(InputSourceContext* ctx, bladerf_channel rx_channel);
static bool bladerf_configure_high_speed_rate_and_rf(InputSourceContext* ctx, bladerf_channel rx_channel);


static InputSourceOps bladerf_ops = {
    .initialize = bladerf_initialize,
    .start_stream = bladerf_start_stream,
    .stop_stream = bladerf_stop_stream,
    .cleanup = bladerf_cleanup,
    .get_summary_info = bladerf_get_summary_info,
    .validate_options = bladerf_validate_options,
    .validate_generic_options = bladerf_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false
};

InputSourceOps* get_bladerf_input_ops(void) {
    return &bladerf_ops;
}

static bool bladerf_validate_generic_options(const AppConfig* config) {
    if (!config->sdr.rf_freq_provided) {
        log_fatal("BladeRF input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool bladerf_validate_options(AppConfig* config) {
    if (s_bladerf_config.bladerf_gain_arg != 0) {
        s_bladerf_config.gain = (int)s_bladerf_config.bladerf_gain_arg;
        s_bladerf_config.gain_provided = true;
    }

    if (s_bladerf_config.bladerf_bandwidth_hz_arg != 0.0f) {
        if (s_bladerf_config.bladerf_bandwidth_hz_arg > UINT_MAX) {
            log_fatal("Value for --bladerf-bandwidth is too large.");
            return false;
        }
        s_bladerf_config.bandwidth_hz = (uint32_t)s_bladerf_config.bladerf_bandwidth_hz_arg;
        s_bladerf_config.bandwidth_provided = true;
    }

    if (s_bladerf_config.channel != 0 && s_bladerf_config.channel != 1) {
        log_fatal("Invalid value for --bladerf-channel. Must be 0 or 1.");
        return false;
    }

    if (s_bladerf_config.bit_depth_arg != 0) {
        s_bladerf_config.bit_depth_provided = true;
    }

    s_bladerf_config.active_bit_depth = 12;

    if (config->sdr.sample_rate_hz > 61440000.0) {
        if (s_bladerf_config.bit_depth_provided && s_bladerf_config.bit_depth_arg == 12) {
            log_error("Invalid configuration: The BladeRF does not support 12-bit mode for sample rates above 61440000 Hz.");
            return false;
        }
        if (!s_bladerf_config.bit_depth_provided) {
             log_warn("Sample rate of %.0f Hz exceeds the 61440000 Hz limit for 12-bit mode. Automatically switching to 8-bit mode.", config->sdr.sample_rate_hz);
        }
        s_bladerf_config.active_bit_depth = 8;
    } else {
        if (s_bladerf_config.bit_depth_provided) {
            if (s_bladerf_config.bit_depth_arg == 8 || s_bladerf_config.bit_depth_arg == 12) {
                s_bladerf_config.active_bit_depth = s_bladerf_config.bit_depth_arg;
            } else {
                log_error("Invalid value for --bladerf-bit-depth. Must be 8 or 12.");
                return false;
            }
        }
    }

    if (s_bladerf_config.active_bit_depth == 8 && s_bladerf_config.bandwidth_provided) {
        log_fatal("Option --bladerf-bandwidth cannot be used with 8-bit high-speed mode.");
        log_error("In this mode, the analog bandwidth is configured automatically by the library.");
        return false;
    }

    return true;
}

static bool bladerf_initialize(InputSourceContext* ctx) {
    AppConfig *config = (AppConfig*)ctx->config;
    AppResources *resources = ctx->resources;
    int status;
    char device_identifier[32];
    bool success = false; // Assume failure until the very end.

    log_info("Attempting to initialize BladeRF device...");

    BladerfPrivateData* private_data = (BladerfPrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(BladerfPrivateData), true);
    if (!private_data) goto cleanup; // mem_arena_alloc logs the error

    // Initialize state variables that the main cleanup function will check.
    private_data->dev = NULL;
    resources->input_module_private_data = private_data;

#if defined(_WIN32) && defined(WITH_BLADERF)
    if (!bladerf_load_api()) goto cleanup;
    if (is_shutdown_requested()) goto cleanup;
#endif

    if (s_bladerf_config.device_index > 0) {
        snprintf(device_identifier, sizeof(device_identifier), "bladerf%d", s_bladerf_config.device_index);
    } else {
        device_identifier[0] = '\0';
    }

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_ERROR);

    status = bladerf_open(&private_data->dev, device_identifier[0] ? device_identifier : NULL);
    if (is_shutdown_requested()) goto cleanup;
    if (status != 0 && status != BLADERF_ERR_UPDATE_FPGA) {
        bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_INFO);
        log_error("Failed to open BladeRF device: %s", bladerf_strerror(status));
        private_data->dev = NULL; // Ensure dev is NULL on failure
        goto cleanup;
    }

    if (s_bladerf_config.fpga_file_path) {
        log_info("Manual FPGA load requested: %s", s_bladerf_config.fpga_file_path);
        status = bladerf_load_fpga(private_data->dev, s_bladerf_config.fpga_file_path);
        if (is_shutdown_requested()) goto cleanup;
        if (status != 0) {
            log_error("Failed to load specified BladeRF FPGA: %s", bladerf_strerror(status));
            goto cleanup;
        }
        log_info("Manual FPGA loaded successfully.");
    } else {
        status = bladerf_is_fpga_configured(private_data->dev);
        if (is_shutdown_requested()) goto cleanup;
        if (status < 0) {
            log_error("Failed to query BladeRF FPGA state: %s", bladerf_strerror(status));
            goto cleanup;
        }
        if (status == 0) {
            log_info("BladeRF FPGA not configured. Attempting to find and load it automatically...");
            if (!bladerf_find_and_load_fpga_automatically(private_data->dev)) {
                goto cleanup;
            }
        } else {
            log_info("BladeRF FPGA is already configured. Proceeding.");
        }
    }

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_INFO);

    const char* board_name_from_api = bladerf_get_board_name(private_data->dev);
    strncpy(private_data->board_name, board_name_from_api, sizeof(private_data->board_name) - 1);
    private_data->board_name[sizeof(private_data->board_name) - 1] = '\0';

    bool is_bladerf2 = (strcmp(private_data->board_name, "bladerf2") == 0);
    if (s_bladerf_config.active_bit_depth == 8 && !is_bladerf2) {
        log_error("Invalid configuration: 8-bit mode is only supported on BladeRF 2.0 devices.");
        goto cleanup;
    }

    struct bladerf_serial serial_struct;
    status = bladerf_get_serial_struct(private_data->dev, &serial_struct);
    if (status != 0) {
        log_warn("Could not retrieve BladeRF serial number: %s", bladerf_strerror(status));
        strncpy(private_data->serial, "????????", sizeof(private_data->serial) - 1);
    } else {
        strncpy(private_data->serial, serial_struct.serial, sizeof(private_data->serial) - 1);
    }
    private_data->serial[sizeof(private_data->serial) - 1] = '\0';

    const char* friendly_name;
    if (is_bladerf2) friendly_name = "Nuand BladeRF 2";
    else if (strcmp(private_data->board_name, "bladerf") == 0) friendly_name = "Nuand BladeRF 1";
    else friendly_name = "Nuand BladeRF";
    snprintf(private_data->display_name, sizeof(private_data->display_name), "%s (S/N: %s)", friendly_name, private_data->serial);
    log_info("Using %s", private_data->display_name);

    bladerf_channel rx_channel;
    if (is_bladerf2) {
        rx_channel = BLADERF_CHANNEL_RX(s_bladerf_config.channel);
    } else {
        rx_channel = BLADERF_CHANNEL_RX(0);
        if (s_bladerf_config.channel != 0) {
            log_warn("Option --bladerf-channel is for BladeRF 2.0 only and is ignored on this BladeRF 1.0 device.");
        }
    }
    
    bool is_high_speed_mode = (config->sdr.sample_rate_hz > 61440000.0);

    if (is_high_speed_mode) {
        if (!is_bladerf2) {
            log_error("Invalid configuration: Sample rates above 61440000 Hz are only supported on BladeRF 2.0 devices.");
            goto cleanup;
        }
        if (!bladerf_configure_high_speed_rate_and_rf(ctx, rx_channel)) goto cleanup;
    } else {
        if (!bladerf_configure_standard_rate_and_rf(ctx, rx_channel)) goto cleanup;
    }

    if (resources->source_info.samplerate == 0) {
        log_fatal("BladeRF failed to set the sample rate. The actual rate was reported as 0 Hz.");
        goto cleanup;
    }

    if (s_bladerf_config.gain_provided) {
        status = bladerf_set_gain_mode(private_data->dev, rx_channel, BLADERF_GAIN_MGC);
        if (status == 0) status = bladerf_set_gain(private_data->dev, rx_channel, s_bladerf_config.gain);
    } else {
        status = bladerf_set_gain_mode(private_data->dev, rx_channel, BLADERF_GAIN_DEFAULT);
    }
    if (is_shutdown_requested()) goto cleanup;
    if (status != 0) {
        log_error("Failed to set BladeRF gain: %s", bladerf_strerror(status));
        goto cleanup;
    }

    if (config->sdr.bias_t_enable) {
        if (is_bladerf2) {
            status = bladerf_set_bias_tee(private_data->dev, rx_channel, true);
            if (is_shutdown_requested()) goto cleanup;
            if (status != 0) {
                log_error("Failed to enable BladeRF Bias-T: %s", bladerf_strerror(status));
                goto cleanup;
            }
        } else {
            log_warn("Bias-T is not supported on this BladeRF model (%s) and will be ignored.", private_data->board_name);
        }
    }

    resources->input_format = (s_bladerf_config.active_bit_depth == 8) ? CS8 : SC16Q11;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);

    size_t buffer_size_bytes = PIPELINE_CHUNK_BASE_SAMPLES * resources->input_bytes_per_sample_pair;
    private_data->stream_temp_buffer = mem_arena_alloc(&resources->setup_arena, buffer_size_bytes, false);
    if (!private_data->stream_temp_buffer) goto cleanup;

    log_info("BladeRF initialized successfully.");
    success = true;

cleanup:
    return success;
}

static bool bladerf_configure_high_speed_rate_and_rf(InputSourceContext* ctx, bladerf_channel rx_channel) {
    AppConfig *config = (AppConfig*)ctx->config;
    AppResources *resources = ctx->resources;
    BladerfPrivateData* private_data = (BladerfPrivateData*)resources->input_module_private_data;
    int status;
    
    log_debug("Enabling BladeRF 2.0 oversample feature for high-speed sampling.");
    status = bladerf_enable_feature(private_data->dev, BLADERF_FEATURE_OVERSAMPLE, true);
    if (status != 0) {
        log_error("Failed to enable BladeRF oversample feature: %s", bladerf_strerror(status));
        return false;
    }

    struct bladerf_rational_rate rate_to_set = { .integer = 0, .num = (uint64_t)config->sdr.sample_rate_hz, .den = 1 };

    struct bladerf_rational_rate actual_rate_from_device;
    status = bladerf_set_rational_sample_rate(private_data->dev, rx_channel, &rate_to_set, &actual_rate_from_device);
    if (status != 0) {
        log_error("Failed to set BladeRF 2.0 rational sample rate: %s", bladerf_strerror(status));
        return false;
    }
    
    if (actual_rate_from_device.den == 0) {
        log_fatal("BladeRF returned an invalid rational sample rate (denominator is zero).");
        return false;
    }
    double actual_rate_double = (double)actual_rate_from_device.integer + ((double)actual_rate_from_device.num / (double)actual_rate_from_device.den);
    resources->source_info.samplerate = (int)actual_rate_double;
    log_info("BladeRF: Requested sample rate %.0f Hz, actual rate set to %d Hz.", config->sdr.sample_rate_hz, resources->source_info.samplerate);

    status = bladerf_set_frequency(private_data->dev, rx_channel, config->sdr.rf_freq_hz);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF frequency: %s", bladerf_strerror(status));
        return false;
    }

    log_info("BladeRF: Bandwidth is set automatically by the library in high-speed mode.");
    return true;
}

static bool bladerf_configure_standard_rate_and_rf(InputSourceContext* ctx, bladerf_channel rx_channel) {
    AppConfig *config = (AppConfig*)ctx->config;
    AppResources *resources = ctx->resources;
    BladerfPrivateData* private_data = (BladerfPrivateData*)resources->input_module_private_data;
    int status;
    
    bladerf_sample_rate requested_rate = (bladerf_sample_rate)config->sdr.sample_rate_hz;
    bladerf_sample_rate actual_rate;
    status = bladerf_set_sample_rate(private_data->dev, rx_channel, requested_rate, &actual_rate);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF sample rate: %s", bladerf_strerror(status));
        return false;
    }
    log_info("BladeRF: Requested sample rate %u Hz, actual rate set to %u Hz.", requested_rate, actual_rate);
    resources->source_info.samplerate = (int)actual_rate;

    bladerf_bandwidth requested_bw = s_bladerf_config.bandwidth_hz;
    bladerf_bandwidth actual_bw;
    status = bladerf_set_bandwidth(private_data->dev, rx_channel, requested_bw, &actual_bw);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF bandwidth: %s", bladerf_strerror(status));
        return false;
    }
    if (requested_bw == 0) log_info("BladeRF: Auto-selected bandwidth: %u Hz.", actual_bw);
    else log_info("BladeRF: Requested bandwidth %u Hz, actual bandwidth set to %u Hz.", requested_bw, actual_bw);
    s_bladerf_config.bandwidth_hz = actual_bw;

    status = bladerf_set_frequency(private_data->dev, rx_channel, config->sdr.rf_freq_hz);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF frequency: %s", bladerf_strerror(status));
        return false;
    }
    return true;
}


static void* bladerf_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    const AppConfig *config = ctx->config;
    BladerfPrivateData* private_data = (BladerfPrivateData*)resources->input_module_private_data;
    int status;
    bladerf_channel rx_channel;
    struct bladerf_metadata meta;

    if (!private_data) {
        return NULL;
    }

    if (resources->source_info.samplerate >= 5000000) {
        log_debug("BladeRF: Using High-Throughput profile for sample rate >= 5 MSPS.");
        s_bladerf_config.num_buffers = BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS;
        s_bladerf_config.buffer_size = BLADERF_PROFILE_HIGHTHROUGHPUT_BUFFER_SIZE;
        s_bladerf_config.num_transfers = BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS;
    } else if (resources->source_info.samplerate >= 1000000) {
        log_debug("BladeRF: Using Balanced profile for sample rate between 1 and 5 MSPS.");
        s_bladerf_config.num_buffers = BLADERF_PROFILE_BALANCED_NUM_BUFFERS;
        s_bladerf_config.buffer_size = BLADERF_PROFILE_BALANCED_BUFFER_SIZE;
        s_bladerf_config.num_transfers = BLADERF_PROFILE_BALANCED_NUM_TRANSFERS;
    } else {
        log_debug("BladeRF: Using Low-Latency profile for sample rate < 1 MSPS.");
        s_bladerf_config.num_buffers = BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS;
        s_bladerf_config.buffer_size = BLADERF_PROFILE_LOWLATENCY_BUFFER_SIZE;
        s_bladerf_config.num_transfers = BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS;
    }

    bladerf_channel_layout layout = BLADERF_RX_X1;
    bladerf_format format;

    if (s_bladerf_config.active_bit_depth == 12) {
        format = BLADERF_FORMAT_SC16_Q11_META;
        log_info("BladeRF: Using 12-bit sample format (SC16Q11).");
    } else {
        format = BLADERF_FORMAT_SC8_Q7_META;
        log_info("BladeRF: Using 8-bit sample format (CS8).");
    }

    status = bladerf_sync_config(private_data->dev, layout, format,
                                 s_bladerf_config.num_buffers, s_bladerf_config.buffer_size,
                                 s_bladerf_config.num_transfers, BLADERF_SYNC_CONFIG_TIMEOUT_MS);
    if (status != 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "bladerf_sync_config() failed: %s", bladerf_strerror(status));
        handle_fatal_thread_error(error_buf, resources);
        return NULL;
    }

    bool is_bladerf2 = (strcmp(private_data->board_name, "bladerf2") == 0);
    rx_channel = is_bladerf2 ? BLADERF_CHANNEL_RX(s_bladerf_config.channel) : BLADERF_CHANNEL_RX(0);

    status = bladerf_enable_module(private_data->dev, rx_channel, true);
    if (status != 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "bladerf_enable_module() failed: %s", bladerf_strerror(status));
        handle_fatal_thread_error(error_buf, resources);
        return NULL;
    }

    if (config->raw_passthrough && resources->input_format != config->output_format) {
        handle_fatal_thread_error("Option --raw-passthrough requires input and output formats to be identical.", resources);
        return NULL;
    }

    unsigned int samples_per_transfer = (unsigned int)(resources->source_info.samplerate * BLADERF_TRANSFER_SIZE_SECONDS);
    if (samples_per_transfer > PIPELINE_CHUNK_BASE_SAMPLES) samples_per_transfer = PIPELINE_CHUNK_BASE_SAMPLES;
    if (samples_per_transfer < 4096) samples_per_transfer = 4096;
    samples_per_transfer = (samples_per_transfer / 1024) * 1024;
    log_debug("BladeRF: Using dynamic transfer size of %u samples.", samples_per_transfer);

    switch (resources->pipeline_mode) {
        case PIPELINE_MODE_BUFFERED_SDR: {
            void* temp_buffer = private_data->stream_temp_buffer;
            if (!temp_buffer) {
                handle_fatal_thread_error("BladeRF: Stream temp buffer is NULL.", resources);
                break;
            }
            while (!is_shutdown_requested() && !resources->error_occurred) {
                memset(&meta, 0, sizeof(meta));
                meta.flags = BLADERF_META_FLAG_RX_NOW;
                status = bladerf_sync_rx(private_data->dev, temp_buffer, samples_per_transfer, &meta, BLADERF_SYNC_RX_TIMEOUT_MS);
                if (status != 0) {
                    if (!is_shutdown_requested()) {
                        char error_buf[256];
                        snprintf(error_buf, sizeof(error_buf), "bladerf_sync_rx() failed: %s", bladerf_strerror(status));
                        handle_fatal_thread_error(error_buf, resources);
                    }
                    break;
                }
                if (meta.actual_count > 0) {
                    if ((meta.status & BLADERF_META_STATUS_OVERRUN) != 0) {
                        log_warn("BladeRF reported a stream overrun (discontinuity). Sending reset event.");
                        sdr_packet_serializer_write_reset_event(resources->sdr_input_buffer);
                    }
                    
                    // --- FIX APPLIED HERE ---
                    // The original code wrote one large packet. This version now calls the
                    // reusable chunking function to correctly packetize the data.
                    sdr_write_interleaved_chunks(
                        resources,
                        (const unsigned char*)temp_buffer,
                        meta.actual_count * resources->input_bytes_per_sample_pair,
                        resources->input_bytes_per_sample_pair,
                        resources->input_format
                    );
                }
            }
            break;
        }

        case PIPELINE_MODE_REALTIME_SDR: {
            if (config->raw_passthrough) {
                void* passthrough_buffer = private_data->stream_temp_buffer;
                if (!passthrough_buffer) {
                    handle_fatal_thread_error("BladeRF: Stream temp buffer is NULL for passthrough.", resources);
                    break;
                }
                while (!is_shutdown_requested() && !resources->error_occurred) {
                    memset(&meta, 0, sizeof(meta));
                    meta.flags = BLADERF_META_FLAG_RX_NOW;
                    status = bladerf_sync_rx(private_data->dev, passthrough_buffer, samples_per_transfer, &meta, BLADERF_SYNC_RX_TIMEOUT_MS);
                    if (status != 0) {
                        if (!is_shutdown_requested()) {
                            char error_buf[256];
                            snprintf(error_buf, sizeof(error_buf), "bladerf_sync_rx() failed: %s", bladerf_strerror(status));
                            handle_fatal_thread_error(error_buf, resources);
                        }
                        break;
                    }
                    if (meta.actual_count > 0) {
                        if ((meta.status & BLADERF_META_STATUS_OVERRUN) != 0) {
                            log_warn("BladeRF reported a stream overrun (discontinuity).");
                        }
                        size_t bytes_to_write = meta.actual_count * resources->input_bytes_per_sample_pair;
                        size_t written = resources->writer_ctx.ops.write(&resources->writer_ctx, passthrough_buffer, bytes_to_write);
                        if (written < bytes_to_write) {
                            log_debug("Real-time passthrough: stdout write error, consumer likely closed pipe.");
                            request_shutdown();
                            break;
                        }
                    }
                }
            } else {
                while (!is_shutdown_requested() && !resources->error_occurred) {
                    SampleChunk *item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                    if (!item) break;

                    memset(&meta, 0, sizeof(meta));
                    meta.flags = BLADERF_META_FLAG_RX_NOW;
                    status = bladerf_sync_rx(private_data->dev, item->raw_input_data, samples_per_transfer, &meta, BLADERF_SYNC_RX_TIMEOUT_MS);
                    
                    if (status != 0) {
                        if (!is_shutdown_requested()) {
                            char error_buf[256];
                            snprintf(error_buf, sizeof(error_buf), "bladerf_sync_rx() failed: %s", bladerf_strerror(status));
                            handle_fatal_thread_error(error_buf, resources);
                        }
                        queue_enqueue(resources->free_sample_chunk_queue, item);
                        break;
                    }

                    item->stream_discontinuity_event = ((meta.status & BLADERF_META_STATUS_OVERRUN) != 0);
                    if (item->stream_discontinuity_event) {
                        log_warn("BladeRF reported a stream overrun (discontinuity).");
                    }
                    item->frames_read = meta.actual_count;
                    item->is_last_chunk = false;

                    item->packet_sample_format = resources->input_format;

                    if (item->frames_read > 0) {
                        pthread_mutex_lock(&resources->progress_mutex);
                        resources->total_frames_read += item->frames_read;
                        pthread_mutex_unlock(&resources->progress_mutex);
                        if (!queue_enqueue(resources->raw_to_pre_process_queue, item)) {
                            queue_enqueue(resources->free_sample_chunk_queue, item);
                            break;
                        }
                    } else {
                        queue_enqueue(resources->free_sample_chunk_queue, item);
                    }
                }
                SampleChunk *last_item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (last_item) {
                    last_item->is_last_chunk = true;
                    last_item->frames_read = 0;
                    queue_enqueue(resources->raw_to_pre_process_queue, last_item);
                }
            }
            break;
        }
        case PIPELINE_MODE_FILE_PROCESSING:
            break;
    }
    
    bladerf_stop_stream(ctx);
    return NULL;
}

static void bladerf_stop_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    BladerfPrivateData* private_data = (BladerfPrivateData*)resources->input_module_private_data;
    if (private_data && private_data->dev) {
        bladerf_channel rx_channel;
        if (strcmp(private_data->board_name, "bladerf2") == 0) {
            rx_channel = BLADERF_CHANNEL_RX(s_bladerf_config.channel);
        } else {
            rx_channel = BLADERF_CHANNEL_RX(0);
        }
        log_debug("Disabling BladeRF RX module...");
        int status = bladerf_enable_module(private_data->dev, rx_channel, false);
        if (status != 0) {
            log_error("Failed to disable BladeRF RX module: %s", bladerf_strerror(status));
        }
    }
}

static void bladerf_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->input_module_private_data) {
        BladerfPrivateData* private_data = (BladerfPrivateData*)resources->input_module_private_data;
        if (private_data->dev) {
            log_info("Closing BladeRF device...");
            bladerf_close(private_data->dev);
        }
        resources->input_module_private_data = NULL;
    }
#if defined(_WIN32) && defined(WITH_BLADERF)
    bladerf_unload_api();
#endif
}

static void bladerf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    BladerfPrivateData* private_data = (BladerfPrivateData*)resources->input_module_private_data;
    add_summary_item(info, "Input Source", "%s", private_data->display_name);

    if (s_bladerf_config.active_bit_depth == 8) add_summary_item(info, "Input Format", "8-bit Signed Complex (cs8)");
    else add_summary_item(info, "Input Format", "12-bit Signed Complex Q4.11 (sc16q11)");

    if (strcmp(private_data->board_name, "bladerf2") == 0) add_summary_item(info, "Channel", "%d (RXA)", s_bladerf_config.channel);
    else add_summary_item(info, "Antenna Port", "Automatic");

    add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);
    add_summary_item(info, "Bandwidth", "%u Hz", s_bladerf_config.bandwidth_hz);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);

    if (s_bladerf_config.gain_provided) add_summary_item(info, "Gain", "%d dB (Manual)", s_bladerf_config.gain);
    else add_summary_item(info, "Gain", "Automatic (AGC)");
    
    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
}

static bool bladerf_find_and_load_fpga_automatically(struct bladerf* dev) {
    int status;
    bladerf_fpga_size fpga_size;
    const char* filename_utf8 = NULL;

    status = bladerf_get_fpga_size(dev, &fpga_size);
    if (is_shutdown_requested()) return false;
    if (status != 0) {
        log_error("Could not determine BladeRF FPGA size: %s", bladerf_strerror(status));
        return false;
    }

    switch (fpga_size) {
        case BLADERF_FPGA_40KLE:  filename_utf8 = "hostedx40.rbf"; break;
        case BLADERF_FPGA_115KLE: filename_utf8 = "hostedx115.rbf"; break;
        case BLADERF_FPGA_A4:     filename_utf8 = "hostedxA4.rbf"; break;
        case BLADERF_FPGA_A5:     filename_utf8 = "hostedxA5.rbf"; break;
        case BLADERF_FPGA_A9:     filename_utf8 = "hostedxA9.rbf"; break;
        default:
            log_error("Unknown or unsupported BladeRF FPGA size (%d). Cannot determine FPGA file.", fpga_size);
            return false;
    }

#ifdef _WIN32
    wchar_t filename_w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, filename_utf8, -1, filename_w, 64) == 0) {
        log_error("Failed to convert FPGA filename to wide char.");
        return false;
    }

    wchar_t exe_path_w[MAX_PATH_BUFFER];
    if (GetModuleFileNameW(NULL, exe_path_w, MAX_PATH_BUFFER) == 0) {
        log_error("Failed to get executable path.");
        return false;
    }
    PathRemoveFileSpecW(exe_path_w);

    wchar_t search_path_w[MAX_PATH_BUFFER];
    PathCchCombine(search_path_w, MAX_PATH_BUFFER, exe_path_w, L"fpga\\bladerf");
    
    wchar_t full_path_w[MAX_PATH_BUFFER];
    PathCchCombine(full_path_w, MAX_PATH_BUFFER, search_path_w, filename_w);

    if (PathFileExistsW(full_path_w)) {
        char full_path_utf8[MAX_PATH_BUFFER];
        if (WideCharToMultiByte(CP_UTF8, 0, full_path_w, -1, full_path_utf8, sizeof(full_path_utf8), NULL, NULL) > 0) {
            log_debug("Found FPGA file at: %s", full_path_utf8);
            status = bladerf_load_fpga(dev, full_path_utf8);
            if (is_shutdown_requested()) return false;
            if (status == 0) {
                log_info("Automatic FPGA loading successful.");
                return true;
            } else {
                log_error("Found FPGA file, but failed to load it: %s", bladerf_strerror(status));
                return false;
            }
        }
    }
#else
    char exe_path_buf[MAX_PATH_BUFFER] = {0};
    char exe_dir[MAX_PATH_BUFFER] = {0};
    char parent_dir_buf[MAX_PATH_BUFFER] = {0};
    
    ssize_t len = readlink("/proc/self/exe", exe_path_buf, sizeof(exe_path_buf) - 1);
    if (len > 0) {
        exe_path_buf[len] = '\0';
        char temp_path1[MAX_PATH_BUFFER];
        snprintf(temp_path1, sizeof(temp_path1), "%s", exe_path_buf);
        snprintf(exe_dir, sizeof(exe_dir), "%s", dirname(temp_path1));
        char temp_path2[MAX_PATH_BUFFER];
        snprintf(temp_path2, sizeof(temp_path2), "%s", exe_path_buf);
        dirname(temp_path2);
        snprintf(parent_dir_buf, sizeof(parent_dir_buf), "%s", dirname(temp_path2));
    } else {
        snprintf(exe_dir, sizeof(exe_dir), ".");
        snprintf(parent_dir_buf, sizeof(parent_dir_buf), "..");
    }

    const char* search_bases[] = { exe_dir, parent_dir_buf, "/usr/local/share/" APP_NAME, "/usr/share/" APP_NAME, NULL };
    char full_path[MAX_PATH_BUFFER];

    for (int i = 0; search_bases[i] != NULL; i++) {
        snprintf(full_path, sizeof(full_path), "%s/fpga/bladerf/%s", search_bases[i], filename_utf8);
        if (access(full_path, F_OK) == 0) {
            log_info("Found FPGA file at: %s", full_path);
            status = bladerf_load_fpga(dev, full_path);
            if (is_shutdown_requested()) return false;
            if (status == 0) {
                log_info("Automatic FPGA load successful.");
                return true;
            } else {
                log_error("Found FPGA file, but failed to load it: %s", bladerf_strerror(status));
                return false;
            }
        }
    }
#endif

    log_error("Could not automatically find the required FPGA file '%s'.", filename_utf8);
    log_error("Please ensure the FPGA files are in the 'fpga/bladerf' subdirectory next to the executable, or installed system-wide.");
    return false;
}
