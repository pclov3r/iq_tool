#include "input_bladerf.h"
#include "module.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "app_context.h"
#include "mem_arena.h"
#include "utils.h"
#include "sample_convert.h"
#include "platform.h"
#include "input_common.h"
#include "queue.h"
#include "packet_serializer.h"
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
    int (*init_stream)(struct bladerf_stream **, struct bladerf *, bladerf_stream_cb, void ***, size_t, bladerf_format, size_t, size_t, void *);
    int (*stream)(struct bladerf_stream *, bladerf_channel_layout);
    int (*submit_stream_buffer)(struct bladerf_stream *, void *, unsigned int);
    void (*deinit_stream)(struct bladerf_stream *);
    int (*set_stream_timeout)(struct bladerf *, bladerf_direction, unsigned int);
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
    LOAD_BLADERF_FUNC(init_stream);
    LOAD_BLADERF_FUNC(stream);
    LOAD_BLADERF_FUNC(submit_stream_buffer);
    LOAD_BLADERF_FUNC(deinit_stream);
    LOAD_BLADERF_FUNC(set_stream_timeout);
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

#define bladerf_log_set_verbosity        bladerf_api.log_set_verbosity
#define bladerf_open                     bladerf_api.open
#define bladerf_close                    bladerf_api.close
#define bladerf_get_board_name           bladerf_api.get_board_name
#define bladerf_get_serial_struct        bladerf_api.get_serial_struct
#define bladerf_is_fpga_configured       bladerf_api.is_fpga_configured
#define bladerf_get_fpga_size            bladerf_api.get_fpga_size
#define bladerf_load_fpga                bladerf_api.load_fpga
#define bladerf_set_sample_rate          bladerf_api.set_sample_rate
#define bladerf_set_rational_sample_rate bladerf_api.set_rational_sample_rate
#define bladerf_enable_feature           bladerf_api.enable_feature
#define bladerf_set_bandwidth            bladerf_api.set_bandwidth
#define bladerf_set_frequency            bladerf_api.set_frequency
#define bladerf_set_gain_mode            bladerf_api.set_gain_mode
#define bladerf_set_gain                 bladerf_api.set_gain
#define bladerf_set_bias_tee             bladerf_api.set_bias_tee
#define bladerf_sync_config              bladerf_api.sync_config
#define bladerf_enable_module            bladerf_api.enable_module
#define bladerf_sync_rx                  bladerf_api.sync_rx
#define bladerf_init_stream              bladerf_api.init_stream
#define bladerf_submit_stream_buffer     bladerf_api.submit_stream_buffer
#define bladerf_deinit_stream            bladerf_api.deinit_stream
#define bladerf_set_stream_timeout       bladerf_api.set_stream_timeout
#define bladerf_strerror                 bladerf_api.strerror
#endif

// Temporarily undefine bladerf_stream macro to allow struct bladerf_stream usage
#if defined(_WIN32) && defined(WITH_BLADERF)
#undef bladerf_stream
#endif

// --- Private Module Configuration ---
static struct {
    int device_index;
    int channel;
    int gain;
    bool gain_provided;
    int bladerf_gain_arg;
    uint32_t bandwidth_hz;
    bool bandwidth_provided;
    float bladerf_bandwidth_hz_arg;
    char *fpga_file_path;
    unsigned int num_buffers;
    unsigned int num_transfers;
    int bit_depth_arg;
    bool bit_depth_provided;
    int active_bit_depth;
} s_bladerf_config;

// --- Private Module State ---
typedef struct {
    struct bladerf *dev;
    struct bladerf_stream *rx_stream;
    char board_name[16];
    char serial[33];
    char display_name[128];
    void **stream_buffers;
    size_t num_stream_buffers;
    size_t samples_per_buffer;
    volatile bool stream_error;
    pthread_mutex_t driver_mutex;
} BladerfContext;


void bladerf_set_default_config(AppConfig* config) {
    config->sdr_general.sample_rate_hz = BLADERF_DEFAULT_SAMPLE_RATE_HZ;
    s_bladerf_config.bandwidth_hz = BLADERF_DEFAULT_BANDWIDTH_HZ;
}

static const struct argparse_option bladerf_input_cli_options[] = {
    OPT_GROUP("BladeRF Input (bladerf)"),
    OPT_INTEGER(0, "bladerf-device-idx", &s_bladerf_config.device_index, "Select specific BladeRF device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_STRING(0, "bladerf-load-fpga", &s_bladerf_config.fpga_file_path, "Load an FPGA bitstream from the specified file.", NULL, 0, 0),
    OPT_FLOAT(0, "bladerf-bandwidth", &s_bladerf_config.bladerf_bandwidth_hz_arg, "Set analog bandwidth in Hz. (Not applicable in 8-bit high-speed mode)", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-gain", &s_bladerf_config.bladerf_gain_arg, "Set overall manual gain in dB. Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-channel", &s_bladerf_config.channel, "For BladeRF 2.0: Select RX channel 0 (RXA) or 1 (RXB). (Default: 0)", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-bit-depth", &s_bladerf_config.bit_depth_arg, "Set capture bit depth {8|12}. 8-bit mode is for BladeRF 2.0 only. (Default: 12, auto-switches to 8 for rates > 61.44 MHz on BladeRF 2.0)", NULL, 0, 0),
};

const struct argparse_option* bladerf_input_get_cli_options(int* count) {
    *count = sizeof(bladerf_input_cli_options) / sizeof(bladerf_input_cli_options[0]);
    return bladerf_input_cli_options;
}

// Forward declarations for static functions
static bool bladerf_input_initialize(ModuleContext* ctx);
static void* bladerf_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx);
static void bladerf_input_stop_stream(ModuleContext* ctx);
static void bladerf_input_cleanup(ModuleContext* ctx);
static void bladerf_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static void* bladerf_rx_stream_callback(struct bladerf *dev, struct bladerf_stream *stream,
                                        struct bladerf_metadata *meta, void *samples,
                                        size_t num_samples, void *user_data);

// Redefine bladerf_stream macro after struct declarations
#if defined(_WIN32) && defined(WITH_BLADERF)
#endif
static bool bladerf_input_validate_options(AppConfig* config);
static bool bladerf_input_validate_generic_options(const AppConfig* config);
static bool bladerf_find_and_load_fpga_automatically(struct bladerf* dev);
static bool bladerf_configure_standard_rate_and_rf(ModuleContext* ctx, bladerf_channel rx_channel);
static bool bladerf_configure_high_speed_rate_and_rf(ModuleContext* ctx, bladerf_channel rx_channel);


static InputModuleInterface s_bladerf_input_api = {
    .initialize = bladerf_input_initialize,
    .start_stream = bladerf_input_start_stream,
    .stop_stream = bladerf_input_stop_stream,
    .cleanup = bladerf_input_cleanup,
    .get_summary_info = bladerf_input_get_summary_info,
    .validate_options = bladerf_input_validate_options,
    .validate_generic_options = bladerf_input_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_bladerf_get_module_api(void) {
    return &s_bladerf_input_api;
}

static bool bladerf_input_validate_generic_options(const AppConfig* config) {
    if (!config->sdr_general.rf_freq_provided) {
        log_fatal("BladeRF input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool bladerf_input_validate_options(AppConfig* config) {
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

    if (config->sdr_general.sample_rate_hz > 61440000.0) {
        if (s_bladerf_config.bit_depth_provided && s_bladerf_config.bit_depth_arg == 12) {
            log_error("Invalid configuration: The BladeRF does not support 12-bit mode for sample rates above 61440000 Hz.");
            return false;
        }
        if (!s_bladerf_config.bit_depth_provided) {
             log_warn("Sample rate of %.0f Hz exceeds the 61440000 Hz limit for 12-bit mode. Automatically switching to 8-bit mode.", config->sdr_general.sample_rate_hz);
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

static bool bladerf_input_initialize(ModuleContext* ctx) {
    AppConfig *config = (AppConfig*)ctx->config;
    AppContext* app = ctx->app;
    int status;
    char device_identifier[32];
    bool success = false; // Assume failure until the very end.

    log_info("Attempting to initialize BladeRF device...");

    BladerfContext* private_data = (BladerfContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(BladerfContext), true);
    if (!private_data) goto cleanup; // mem_arena_alloc logs the error

    // Initialize state variables that the main cleanup function will check.
    private_data->dev = NULL;

    if (pthread_mutex_init(&private_data->driver_mutex, NULL) != 0) {
        log_error("Failed to init driver mutex.");
        return false;
    }
    app->module.input_private_data = private_data;

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

    bool is_high_speed_mode = (config->sdr_general.sample_rate_hz > 61440000.0);

    if (is_high_speed_mode) {
        if (!is_bladerf2) {
            log_error("Invalid configuration: Sample rates above 61440000 Hz are only supported on BladeRF 2.0 devices.");
            goto cleanup;
        }
        if (!bladerf_configure_high_speed_rate_and_rf(ctx, rx_channel)) goto cleanup;
    } else {
        if (!bladerf_configure_standard_rate_and_rf(ctx, rx_channel)) goto cleanup;
    }

    if (app->module.source_info.sample_rate == 0) {
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

    if (config->sdr_general.bias_t_enable) {
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

    app->module.input_format = (s_bladerf_config.active_bit_depth == 8) ? CS8 : SC16Q11;
    app->module.input_bytes_per_sample_pair = sample_convert_bytes_per_sample(app->module.input_format);

    private_data->rx_stream = NULL;
    private_data->stream_buffers = NULL;
    private_data->stream_error = false;

    log_info("BladeRF initialized successfully.");
    app->pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT;
    success = true;

cleanup:
    return success;
}

static bool bladerf_configure_high_speed_rate_and_rf(ModuleContext* ctx, bladerf_channel rx_channel) {
    AppConfig *config = (AppConfig*)ctx->config;
    AppContext* app = ctx->app;
    BladerfContext* private_data = (BladerfContext*)app->module.input_private_data;
    int status;

    log_debug("Enabling BladeRF 2.0 oversample feature for high-speed sampling.");
    status = bladerf_enable_feature(private_data->dev, BLADERF_FEATURE_OVERSAMPLE, true);
    if (status != 0) {
        log_error("Failed to enable BladeRF oversample feature: %s", bladerf_strerror(status));
        return false;
    }

    struct bladerf_rational_rate rate_to_set = { .integer = 0, .num = (uint64_t)config->sdr_general.sample_rate_hz, .den = 1 };

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
    app->module.source_info.sample_rate = (int)actual_rate_double;
    log_info("BladeRF: Requested sample rate %.0f Hz, actual rate set to %d Hz.", config->sdr_general.sample_rate_hz, app->module.source_info.sample_rate);

    status = bladerf_set_frequency(private_data->dev, rx_channel, config->sdr_general.rf_freq_hz);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF frequency: %s", bladerf_strerror(status));
        return false;
    }

    log_info("BladeRF: Bandwidth is set automatically by the library in high-speed mode.");
    return true;
}

static bool bladerf_configure_standard_rate_and_rf(ModuleContext* ctx, bladerf_channel rx_channel) {
    AppConfig *config = (AppConfig*)ctx->config;
    AppContext* app = ctx->app;
    BladerfContext* private_data = (BladerfContext*)app->module.input_private_data;
    int status;

    bladerf_sample_rate requested_rate = (bladerf_sample_rate)config->sdr_general.sample_rate_hz;
    bladerf_sample_rate actual_rate;
    status = bladerf_set_sample_rate(private_data->dev, rx_channel, requested_rate, &actual_rate);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF sample rate: %s", bladerf_strerror(status));
        return false;
    }
    log_info("BladeRF: Requested sample rate %u Hz, actual rate set to %u Hz.", requested_rate, actual_rate);
    app->module.source_info.sample_rate = (int)actual_rate;

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

    status = bladerf_set_frequency(private_data->dev, rx_channel, config->sdr_general.rf_freq_hz);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF frequency: %s", bladerf_strerror(status));
        return false;
    }
    return true;
}

static void* bladerf_rx_stream_callback(struct bladerf *dev, struct bladerf_stream *stream,
                                        struct bladerf_metadata *meta, void *samples,
                                        size_t num_samples, void *user_data) {
    (void)dev;
    (void)stream;

    AppContext* app = (AppContext*)user_data;
    BladerfContext* private_data = (BladerfContext*)app->module.input_private_data;

    if (!samples || num_samples == 0) {
        return BLADERF_STREAM_NO_DATA;
    }

    if (is_shutdown_requested() || app->stats.error_occurred || private_data->stream_error) {
        return BLADERF_STREAM_SHUTDOWN;
    }

    if (meta && (meta->status & BLADERF_META_STATUS_OVERRUN) != 0) {
        log_warn("BladeRF reported a stream overrun. Sending reset event.");
        packet_serializer_write_reset_event(app->pipeline.sdr_input_buffer);
    }

    if (!app->module.queue_samples(app->module.pipeline_ctx, samples, num_samples, app->module.input_format)) {
        log_warn("SDR input buffer overrun! Dropped data.");
    }

    for (size_t i = 0; i < private_data->num_stream_buffers; i++) {
        if (private_data->stream_buffers[i] != samples) {
            return private_data->stream_buffers[i];
        }
    }

    return private_data->stream_buffers[0];
}

static void* bladerf_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx) {
    ctx->app->module.queue_samples = queue_samples;
    ctx->app->module.pipeline_ctx = pipeline_ctx;
    AppContext* app = ctx->app;
    const AppConfig *config = ctx->config;
    BladerfContext* private_data = (BladerfContext*)app->module.input_private_data;
    int status;
    bladerf_channel rx_channel;

    if (!private_data) return NULL;

    if (app->module.source_info.sample_rate >= 5000000) {
        log_debug("BladeRF: Using High-Throughput profile for sample rate >= 5 MSPS.");
        s_bladerf_config.num_buffers = BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS;
        s_bladerf_config.num_transfers = BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS;
    } else if (app->module.source_info.sample_rate >= 1000000) {
        log_debug("BladeRF: Using Balanced profile for sample rate between 1 and 5 MSPS.");
        s_bladerf_config.num_buffers = BLADERF_PROFILE_BALANCED_NUM_BUFFERS;
        s_bladerf_config.num_transfers = BLADERF_PROFILE_BALANCED_NUM_TRANSFERS;
    } else {
        log_debug("BladeRF: Using Low-Latency profile for sample rate < 1 MSPS.");
        s_bladerf_config.num_buffers = BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS;
        s_bladerf_config.num_transfers = BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS;
    }

    bool is_bladerf2 = (strcmp(private_data->board_name, "bladerf2") == 0);
    rx_channel = is_bladerf2 ? BLADERF_CHANNEL_RX(s_bladerf_config.channel) : BLADERF_CHANNEL_RX(0);

    if (config->dsp.raw_passthrough && app->module.input_format != config->output.format) {
        handle_fatal_thread_error("Option --raw-passthrough requires input and output formats to be identical.", app);
        return NULL;
    }

    bladerf_format format;
    if (s_bladerf_config.active_bit_depth == 12) {
        format = BLADERF_FORMAT_SC16_Q11;
    } else {
        format = BLADERF_FORMAT_SC8_Q7;
    }

    unsigned int samples_per_buffer = (unsigned int)(app->module.source_info.sample_rate * BLADERF_TRANSFER_SIZE_SECONDS);

    // Apply limits and alignment
    if (samples_per_buffer > 65536) samples_per_buffer = 65536;
    samples_per_buffer = (samples_per_buffer / 2048) * 2048;
    if (samples_per_buffer < 2048) samples_per_buffer = 2048;

    log_info("BladeRF: Driver transfer size aligned to %u samples.", samples_per_buffer);

    private_data->samples_per_buffer = samples_per_buffer;
    private_data->num_stream_buffers = s_bladerf_config.num_buffers;

    status = bladerf_init_stream(&private_data->rx_stream, private_data->dev,
                                 bladerf_rx_stream_callback, &private_data->stream_buffers,
                                 s_bladerf_config.num_buffers, format,
                                 samples_per_buffer, s_bladerf_config.num_transfers,
                                 app);
    if (status != 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "bladerf_init_stream() failed: %s", bladerf_strerror(status));
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    status = bladerf_enable_module(private_data->dev, rx_channel, true);
    if (status != 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "bladerf_enable_module() failed: %s", bladerf_strerror(status));
        handle_fatal_thread_error(error_buf, app);
        bladerf_deinit_stream(private_data->rx_stream);
        private_data->rx_stream = NULL;
        return NULL;
    }

    bladerf_channel_layout layout = BLADERF_RX_X1;
    #if defined(_WIN32) && defined(WITH_BLADERF)
    status = bladerf_api.stream(private_data->rx_stream, layout);
#else
    status = bladerf_stream(private_data->rx_stream, layout);
#endif

    if (status != 0 && !is_shutdown_requested()) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "bladerf_stream() failed: %s", bladerf_strerror(status));
        handle_fatal_thread_error(error_buf, app);
    }

    bladerf_input_stop_stream(ctx);
    return NULL;
}

static void bladerf_input_stop_stream(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    BladerfContext* private_data = (BladerfContext*)app->module.input_private_data;
    if (private_data && private_data->dev) {
        // Set error flag so callback returns BLADERF_STREAM_SHUTDOWN
        private_data->stream_error = true;

        // Give the stream thread time to exit cleanly
#ifdef _WIN32
        Sleep(200);
#else
        usleep(200000);
#endif

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

static void bladerf_input_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (app->module.input_private_data) {
        BladerfContext* private_data = (BladerfContext*)app->module.input_private_data;
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->rx_stream) {
            log_debug("Deinitializing BladeRF stream...");
            bladerf_deinit_stream(private_data->rx_stream);
            private_data->rx_stream = NULL;
        }
        if (private_data->dev) {
            log_info("Closing BladeRF device...");
            bladerf_close(private_data->dev);
        }
        pthread_mutex_unlock(&private_data->driver_mutex);
        pthread_mutex_destroy(&private_data->driver_mutex);
        app->module.input_private_data = NULL;
    }
#if defined(_WIN32) && defined(WITH_BLADERF)
    bladerf_unload_api();
#endif
}

static void bladerf_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    AppContext* app = ctx->app;
    BladerfContext* private_data = (BladerfContext*)app->module.input_private_data;
    add_summary_item(info, "Input Source", "%s", private_data->display_name);

    if (s_bladerf_config.active_bit_depth == 8) add_summary_item(info, "Input Format", "8-bit Signed Complex (cs8)");
    else add_summary_item(info, "Input Format", "12-bit Signed Complex Q4.11 (sc16q11)");

    if (strcmp(private_data->board_name, "bladerf2") == 0) add_summary_item(info, "Channel", "%d (RXA)", s_bladerf_config.channel);
    else add_summary_item(info, "Antenna Port", "Automatic");

    add_summary_item(info, "Input Rate", "%d Hz", app->module.source_info.sample_rate);
    add_summary_item(info, "Bandwidth", "%u Hz", s_bladerf_config.bandwidth_hz);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr_general.rf_freq_hz);

    if (s_bladerf_config.gain_provided) add_summary_item(info, "Gain", "%d dB (Manual)", s_bladerf_config.gain);
    else add_summary_item(info, "Gain", "Automatic (AGC)");

    add_summary_item(info, "Bias-T", "%s", config->sdr_general.bias_t_enable ? "Enabled" : "Disabled");
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
