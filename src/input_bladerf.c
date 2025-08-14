#include "input_bladerf.h"
#include "log.h"
#include "signal_handler.h"
#include "config.h"
#include "types.h"
#include "utils.h"
#include "sample_convert.h"
#include "platform.h"
#include "input_common.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include "argparse.h"

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
BladerfApiFunctionPointers bladerf_api;

#define LOAD_BLADERF_FUNC(func_name) \
    do { \
        FARPROC proc = GetProcAddress(bladerf_api.dll_handle, "bladerf_" #func_name); \
        if (!proc) { \
            log_fatal("Failed to load BladeRF API function: %s", "bladerf_" #func_name); \
            FreeLibrary(bladerf_api.dll_handle); \
            bladerf_api.dll_handle = NULL; \
            return false; \
        } \
        /* Use memcpy to safely convert the generic function pointer to the specific one. */ \
        /* This is the standard-compliant way to avoid the -Wpedantic warnings. */ \
        memcpy(&bladerf_api.func_name, &proc, sizeof(bladerf_api.func_name)); \
    } while (0)


bool bladerf_load_api(void) {
    if (bladerf_api.dll_handle) { return true; }
    log_info("Attempting to load bladeRF.dll...");
    bladerf_api.dll_handle = LoadLibraryA("bladeRF.dll");
    if (!bladerf_api.dll_handle) {
        print_win_error("LoadLibraryA for bladeRF.dll", GetLastError());
        log_error("Please ensure the BladeRF driver/library is installed and its directory is in the system PATH.");
        return false;
    }
    log_info("BladeRF DLL loaded successfully. Loading function pointers...");
    LOAD_BLADERF_FUNC(log_set_verbosity);
    LOAD_BLADERF_FUNC(open);
    LOAD_BLADERF_FUNC(close);
    LOAD_BLADERF_FUNC(get_board_name);
    LOAD_BLADERF_FUNC(get_serial);
    LOAD_BLADERF_FUNC(is_fpga_configured);
    LOAD_BLADERF_FUNC(get_fpga_size);
    LOAD_BLADERF_FUNC(load_fpga);
    LOAD_BLADERF_FUNC(set_sample_rate);
    LOAD_BLADERF_FUNC(set_bandwidth);
    LOAD_BLADERF_FUNC(set_frequency);
    LOAD_BLADERF_FUNC(set_gain_mode);
    LOAD_BLADERF_FUNC(set_gain);
    LOAD_BLADERF_FUNC(set_bias_tee);
    LOAD_BLADERF_FUNC(sync_config);
    LOAD_BLADERF_FUNC(enable_module);
    LOAD_BLADERF_FUNC(sync_rx);
    LOAD_BLADERF_FUNC(strerror);
    log_info("All BladeRF API function pointers loaded.");
    return true;
}

void bladerf_unload_api(void) {
    if (bladerf_api.dll_handle) {
        FreeLibrary(bladerf_api.dll_handle);
        bladerf_api.dll_handle = NULL;
        log_info("BladeRF API DLL unloaded.");
    }
}
#endif

extern AppConfig g_config;

void bladerf_set_default_config(AppConfig* config) {
    config->bladerf.sample_rate_hz = BLADERF_DEFAULT_SAMPLE_RATE_HZ;
    config->bladerf.bandwidth_hz = BLADERF_DEFAULT_BANDWIDTH_HZ;
}

static const struct argparse_option bladerf_cli_options[] = {
    OPT_GROUP("BladeRF-Specific Options"),
    OPT_INTEGER(0, "bladerf-device-idx", &g_config.bladerf.device_index, "Select specific BladeRF device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_STRING(0, "bladerf-load-fpga", &g_config.bladerf.fpga_file_path, "Load an FPGA bitstream from the specified file.", NULL, 0, 0),
    OPT_FLOAT(0, "bladerf-sample-rate", &g_config.bladerf.bladerf_sample_rate_hz_arg, "Set sample rate in Hz.", NULL, 0, 0),
    OPT_FLOAT(0, "bladerf-bandwidth", &g_config.bladerf.bladerf_bandwidth_hz_arg, "Set analog bandwidth in Hz. (Default: Auto-selected)", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-gain", &g_config.bladerf.bladerf_gain_arg, "Set overall manual gain in dB. Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "bladerf-channel", &g_config.bladerf.channel, "For BladeRF 2.0: Select RX channel 0 (RXA) or 1 (RXB). (Default: 0)", NULL, 0, 0),
};

const struct argparse_option* bladerf_get_cli_options(int* count) {
    *count = sizeof(bladerf_cli_options) / sizeof(bladerf_cli_options[0]);
    return bladerf_cli_options;
}

static bool bladerf_initialize(InputSourceContext* ctx);
static void* bladerf_start_stream(InputSourceContext* ctx);
static void bladerf_stop_stream(InputSourceContext* ctx);
static void bladerf_cleanup(InputSourceContext* ctx);
static void bladerf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool bladerf_validate_options(AppConfig* config);
static bool bladerf_find_and_load_fpga_automatically(struct bladerf* dev);

#ifndef _WIN32
static int get_usbfs_memory_mb_for_linux(void);
#endif

static InputSourceOps bladerf_ops = {
    .initialize = bladerf_initialize,
    .start_stream = bladerf_start_stream,
    .stop_stream = bladerf_stop_stream,
    .cleanup = bladerf_cleanup,
    .get_summary_info = bladerf_get_summary_info,
    .validate_options = bladerf_validate_options,
    .has_known_length = _input_source_has_known_length_false
};

InputSourceOps* get_bladerf_input_ops(void) {
    return &bladerf_ops;
}

static bool bladerf_validate_options(AppConfig* config) {
    if (config->bladerf.bladerf_gain_arg != 0) {
        config->bladerf.gain = (int)config->bladerf.bladerf_gain_arg;
        config->bladerf.gain_provided = true;
    }

    if (config->bladerf.bladerf_sample_rate_hz_arg != 0.0f) {
        if (config->bladerf.bladerf_sample_rate_hz_arg > UINT_MAX) {
            log_fatal("Value for --bladerf-sample-rate is too large.");
            return false;
        }
        config->bladerf.sample_rate_hz = (uint32_t)config->bladerf.bladerf_sample_rate_hz_arg;
        config->bladerf.sample_rate_provided = true;
    } else {
        log_fatal("Missing required argument --bladerf-sample-rate <hz>.");
        return false;
    }

    if (config->bladerf.bladerf_bandwidth_hz_arg != 0.0f) {
        if (config->bladerf.bladerf_bandwidth_hz_arg > UINT_MAX) {
            log_fatal("Value for --bladerf-bandwidth is too large.");
            return false;
        }
        config->bladerf.bandwidth_hz = (uint32_t)config->bladerf.bladerf_bandwidth_hz_arg;
        config->bladerf.bandwidth_provided = true;
    }

    if (config->bladerf.channel != 0 && config->bladerf.channel != 1) {
        log_fatal("Invalid value for --bladerf-channel. Must be 0 or 1.");
        return false;
    }

    return true;
}

#ifndef _WIN32
static int get_usbfs_memory_mb_for_linux(void) {
    const char* path = "/sys/module/usbcore/parameters/usbfs_memory_mb";
    FILE* fp = fopen(path, "r");
    if (!fp) {
        log_warn("Could not read USB memory limit from sysfs. Assuming default of 16 MB.");
        return 16;
    }
    char buf[32];
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        log_warn("Could not read value from usbfs_memory_mb file. Assuming default of 16 MB.");
        return 16;
    }
    fclose(fp);
    int usbfs_memory_mb = atoi(buf);
    if (usbfs_memory_mb <= 0) {
        log_warn("Invalid value '%s' in usbfs_memory_mb file. Assuming default of 16 MB.", buf);
        return 16;
    }
    return usbfs_memory_mb;
}
#endif

static bool bladerf_initialize(InputSourceContext* ctx) {
    AppConfig *config = (AppConfig*)ctx->config;
    AppResources *resources = ctx->resources;
    struct bladerf* dev = NULL;
    int status;
    char device_identifier[32];

    log_info("Attempting to initialize BladeRF device...");

#if defined(_WIN32) && defined(WITH_BLADERF)
    if (!bladerf_load_api()) { return false; }
    if (is_shutdown_requested()) { return false; }
#endif

    if (config->bladerf.device_index > 0) {
        snprintf(device_identifier, sizeof(device_identifier), "bladerf%d", config->bladerf.device_index);
    } else {
        device_identifier[0] = '\0';
    }

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_ERROR);

    status = bladerf_open(&dev, device_identifier[0] ? device_identifier : NULL);
    resources->bladerf_dev = dev;
    if (is_shutdown_requested()) { return false; }

    if (status != 0 && status != BLADERF_ERR_UPDATE_FPGA) {
        bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_INFO);
        log_error("Failed to open BladeRF device: %s", bladerf_strerror(status));
        return false;
    }

    if (config->bladerf.fpga_file_path) {
        log_info("Manual FPGA load requested: %s", config->bladerf.fpga_file_path);
        status = bladerf_load_fpga(dev, config->bladerf.fpga_file_path);
        if (is_shutdown_requested()) { return false; }
        if (status != 0) {
            log_error("Failed to load specified BladeRF FPGA: %s", bladerf_strerror(status));
            return false;
        }
        log_info("Manual FPGA loaded successfully.");
    } else {
        status = bladerf_is_fpga_configured(dev);
        if (is_shutdown_requested()) { return false; }
        if (status < 0) {
            log_error("Failed to query BladeRF FPGA state: %s", bladerf_strerror(status));
            return false;
        }
        if (status == 0) {
            log_info("BladeRF FPGA not configured. Attempting to find and load it automatically...");
            if (!bladerf_find_and_load_fpga_automatically(dev)) {
                return false;
            }
        } else {
            log_info("BladeRF FPGA is already configured. Proceeding.");
        }
    }

    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_INFO);

    const char* board_name_from_api = bladerf_get_board_name(dev);
    strncpy(resources->bladerf_board_name, board_name_from_api, sizeof(resources->bladerf_board_name) - 1);
    resources->bladerf_board_name[sizeof(resources->bladerf_board_name) - 1] = '\0';

    bladerf_get_serial(dev, resources->bladerf_serial);

    const char* friendly_name;
    if (strcmp(resources->bladerf_board_name, "bladerf2") == 0) {
        friendly_name = "Nuand BladeRF 2";
    } else if (strcmp(resources->bladerf_board_name, "bladerf") == 0) {
        friendly_name = "Nuand BladeRF 1";
    } else {
        friendly_name = "Nuand BladeRF";
    }
    snprintf(resources->bladerf_display_name, sizeof(resources->bladerf_display_name),
             "%s (S/N: %s)", friendly_name, resources->bladerf_serial);

    log_info("Using %s", resources->bladerf_display_name);

    bladerf_channel rx_channel;
    if (strcmp(resources->bladerf_board_name, "bladerf2") == 0) {
        rx_channel = BLADERF_CHANNEL_RX(config->bladerf.channel);
    } else {
        rx_channel = BLADERF_CHANNEL_RX(0);
        if (config->bladerf.channel != 0) {
            log_warn("Option --bladerf-channel is for BladeRF 2.0 only and is ignored on this BladeRF 1.0 device.");
        }
    }

    bladerf_sample_rate requested_rate = config->bladerf.sample_rate_hz;
    bladerf_sample_rate actual_rate;
    status = bladerf_set_sample_rate(dev, rx_channel, requested_rate, &actual_rate);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF sample rate: %s", bladerf_strerror(status));
        return false;
    }
    log_info("BladeRF: Requested sample rate %u Hz, actual rate set to %u Hz.", requested_rate, actual_rate);
    resources->source_info.samplerate = (int)actual_rate;

    if (actual_rate > 40000000) {
#ifdef _WIN32
        log_debug("BladeRF: Using High-Throughput (Optimized) profile for sample rate > 40 MSPS.");
        log_debug("BladeRF: (Windows Optimization: Using large 128K transfer buffers).");
        config->bladerf.num_buffers = BLADERF_PROFILE_WIN_OPTIMIZED_NUM_BUFFERS;
        config->bladerf.buffer_size = BLADERF_PROFILE_WIN_OPTIMIZED_BUFFER_SIZE;
        config->bladerf.num_transfers = BLADERF_PROFILE_WIN_OPTIMIZED_NUM_TRANSFERS;
#else
        int detected_mb = get_usbfs_memory_mb_for_linux();
        log_debug("BladeRF: Using High-Throughput (Optimized) profile for sample rate > 40 MSPS.");
        log_debug("BladeRF: (Linux Optimization: Dynamically calculating profile based on %d MB usbfs limit).", detected_mb);

        long long memory_budget = (long long)detected_mb * 1024 * 1024 * BLADERF_PROFILE_LINUX_MEM_BUDGET_FACTOR;
        unsigned int calculated_transfers = memory_budget / BLADERF_PROFILE_LINUX_OPTIMIZED_BUFFER_SIZE;

        if (calculated_transfers > BLADERF_PROFILE_LINUX_MAX_TRANSFERS) {
            calculated_transfers = BLADERF_PROFILE_LINUX_MAX_TRANSFERS;
        }
        if (calculated_transfers < 16) {
            calculated_transfers = 16;
        }

        config->bladerf.buffer_size = BLADERF_PROFILE_LINUX_OPTIMIZED_BUFFER_SIZE;
        config->bladerf.num_transfers = calculated_transfers;
        config->bladerf.num_buffers = calculated_transfers;
        log_debug("BladeRF: Calculated profile: num_buffers=%u, buffer_size=%u, num_transfers=%u",
                 config->bladerf.num_buffers, config->bladerf.buffer_size, config->bladerf.num_transfers);
#endif
    } else if (actual_rate >= 5000000) {
        log_debug("BladeRF: Using High-Throughput profile for sample rate between 5 and 40 MSPS.");
        config->bladerf.num_buffers = BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS;
        config->bladerf.buffer_size = BLADERF_PROFILE_HIGHTHROUGHPUT_BUFFER_SIZE;
        config->bladerf.num_transfers = BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS;
    } else if (actual_rate >= 1000000) {
        log_debug("BladeRF: Using Balanced profile for sample rate between 1 and 5 MSPS.");
        config->bladerf.num_buffers = BLADERF_PROFILE_BALANCED_NUM_BUFFERS;
        config->bladerf.buffer_size = BLADERF_PROFILE_BALANCED_BUFFER_SIZE;
        config->bladerf.num_transfers = BLADERF_PROFILE_BALANCED_NUM_TRANSFERS;
    } else {
        log_debug("BladeRF: Using Low-Latency profile for sample rate < 1 MSPS.");
        config->bladerf.num_buffers = BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS;
        config->bladerf.buffer_size = BLADERF_PROFILE_LOWLATENCY_BUFFER_SIZE;
        config->bladerf.num_transfers = BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS;
    }

    bladerf_bandwidth requested_bw = config->bladerf.bandwidth_hz;
    bladerf_bandwidth actual_bw;
    status = bladerf_set_bandwidth(dev, rx_channel, requested_bw, &actual_bw);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF bandwidth: %s", bladerf_strerror(status));
        return false;
    }
    if (requested_bw == 0) {
        log_info("BladeRF: Auto-selected bandwidth: %u Hz.", actual_bw);
    } else {
        log_info("BladeRF: Requested bandwidth %u Hz, actual bandwidth set to %u Hz.", requested_bw, actual_bw);
    }
    config->bladerf.bandwidth_hz = actual_bw;

    status = bladerf_set_frequency(dev, rx_channel, config->sdr.rf_freq_hz);
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF frequency: %s", bladerf_strerror(status));
        return false;
    }

    if (config->bladerf.gain_provided) {
        status = bladerf_set_gain_mode(dev, rx_channel, BLADERF_GAIN_MGC);
        if (status == 0) status = bladerf_set_gain(dev, rx_channel, config->bladerf.gain);
    } else {
        status = bladerf_set_gain_mode(dev, rx_channel, BLADERF_GAIN_DEFAULT);
    }
    if (is_shutdown_requested()) { return false; }
    if (status != 0) {
        log_error("Failed to set BladeRF gain: %s", bladerf_strerror(status));
        return false;
    }

    if (config->sdr.bias_t_enable) {
        if (strcmp(resources->bladerf_board_name, "bladerf2") == 0) {
            status = bladerf_set_bias_tee(dev, rx_channel, true);
            if (is_shutdown_requested()) { return false; }
            if (status != 0) {
                log_error("Failed to enable BladeRF Bias-T: %s", bladerf_strerror(status));
                return false;
            }
        } else {
            log_warn("Bias-T is not supported on this BladeRF model (%s) and will be ignored.", resources->bladerf_board_name);
        }
    }

    resources->input_format = SC16Q11;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    resources->bladerf_initialized_successfully = true;
    log_info("BladeRF initialized successfully.");
    return true;
}

static void* bladerf_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    const AppConfig *config = ctx->config;
    struct bladerf* dev = resources->bladerf_dev;
    int status;
    bladerf_channel_layout layout;
    bladerf_channel rx_channel;
    struct bladerf_metadata meta;

    if (!resources->bladerf_initialized_successfully) {
        return NULL;
    }

    if (strcmp(resources->bladerf_board_name, "bladerf2") == 0) {
        layout = BLADERF_RX_X1;
        rx_channel = BLADERF_CHANNEL_RX(config->bladerf.channel);
    } else {
        layout = BLADERF_RX_X1;
        rx_channel = BLADERF_CHANNEL_RX(0);
    }

    status = bladerf_sync_config(dev, layout, BLADERF_FORMAT_SC16_Q11_META,
                                 config->bladerf.num_buffers,
                                 config->bladerf.buffer_size,
                                 config->bladerf.num_transfers,
                                 BLADERF_SYNC_CONFIG_TIMEOUT_MS);
    if (status != 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "bladerf_sync_config() failed: %s", bladerf_strerror(status));
        handle_fatal_thread_error(error_buf, resources);
        return NULL;
    }

    status = bladerf_enable_module(dev, rx_channel, true);
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
    if (samples_per_transfer > BUFFER_SIZE_SAMPLES) {
        samples_per_transfer = BUFFER_SIZE_SAMPLES;
    }
    if (samples_per_transfer < 4096) {
        samples_per_transfer = 4096;
    }
    samples_per_transfer = (samples_per_transfer / 1024) * 1024;
    log_debug("BladeRF: Using dynamic transfer size of %u samples.", samples_per_transfer);

    while (!is_shutdown_requested() && !resources->error_occurred) {
        SampleChunk *item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!item) break;

        memset(&meta, 0, sizeof(meta));
        meta.flags = BLADERF_META_FLAG_RX_NOW;

        void* target_buffer = config->raw_passthrough ? item->final_output_data : item->raw_input_data;
        status = bladerf_sync_rx(dev, target_buffer, samples_per_transfer, &meta, BLADERF_SYNC_RX_TIMEOUT_MS);
        
        if (status != 0) {
            if (!is_shutdown_requested()) {
                char error_buf[256];
                snprintf(error_buf, sizeof(error_buf), "bladerf_sync_rx() failed: %s", bladerf_strerror(status));
                handle_fatal_thread_error(error_buf, resources);
            }
            queue_enqueue(resources->free_sample_chunk_queue, item);
            break;
        }

        if ((meta.status & BLADERF_META_STATUS_OVERRUN) != 0) {
            log_warn("BladeRF reported a stream overrun (discontinuity).");
            log_info("Signaling pipeline to reset DSP state.");
            item->stream_discontinuity_event = true;
        } else {
            item->stream_discontinuity_event = false;
        }

        item->frames_read = meta.actual_count;
        item->is_last_chunk = false;

        pthread_mutex_lock(&resources->progress_mutex);
        resources->total_frames_read += item->frames_read;
        pthread_mutex_unlock(&resources->progress_mutex);

        if (config->raw_passthrough) {
            item->frames_to_write = item->frames_read;
            if (!queue_enqueue(resources->final_output_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }
        } else {
            if (!queue_enqueue(resources->raw_to_pre_process_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }
        }
    }
    return NULL;
}

static void bladerf_stop_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    const AppConfig *config = ctx->config;
    if (resources->bladerf_dev && resources->bladerf_initialized_successfully) {
        bladerf_channel rx_channel;
        if (strcmp(resources->bladerf_board_name, "bladerf2") == 0) {
            rx_channel = BLADERF_CHANNEL_RX(config->bladerf.channel);
        } else {
            rx_channel = BLADERF_CHANNEL_RX(0);
        }
        log_info("Disabling BladeRF RX module...");
        int status = bladerf_enable_module(resources->bladerf_dev, rx_channel, false);
        if (status != 0) {
            log_error("Failed to disable BladeRF RX module: %s", bladerf_strerror(status));
        }
    }
}

static void bladerf_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->bladerf_dev) {
        log_info("Closing BladeRF device...");
        bladerf_close(resources->bladerf_dev);
    }
    resources->bladerf_dev = NULL;
#if defined(_WIN32) && defined(WITH_BLADERF)
    bladerf_unload_api();
#endif
}

static void bladerf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    add_summary_item(info, "Input Source", "%s", resources->bladerf_display_name);
    add_summary_item(info, "Input Format", "16-bit Signed Complex Q4.11 (sc16q11)");

    if (strcmp(resources->bladerf_board_name, "bladerf2") == 0) {
        add_summary_item(info, "Channel", "%d (RXA)", config->bladerf.channel);
    } else {
        add_summary_item(info, "Antenna Port", "Automatic");
    }

    add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);
    add_summary_item(info, "Bandwidth", "%u Hz", config->bladerf.bandwidth_hz);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);
    if (config->bladerf.gain_provided) {
        add_summary_item(info, "Gain", "%d dB (Manual)", config->bladerf.gain);
    } else {
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }
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

    wchar_t exe_path_w[MAX_PATH_LEN];
    if (GetModuleFileNameW(NULL, exe_path_w, MAX_PATH_LEN) == 0) {
        log_error("Failed to get executable path.");
        return false;
    }
    PathRemoveFileSpecW(exe_path_w);

    wchar_t search_path_w[MAX_PATH_LEN];
    PathCchCombine(search_path_w, MAX_PATH_LEN, exe_path_w, L"fpga\\bladerf");
    
    wchar_t full_path_w[MAX_PATH_LEN];
    PathCchCombine(full_path_w, MAX_PATH_LEN, search_path_w, filename_w);

    if (PathFileExistsW(full_path_w)) {
        char full_path_utf8[MAX_PATH_LEN];
        if (WideCharToMultiByte(CP_UTF8, 0, full_path_w, -1, full_path_utf8, sizeof(full_path_utf8), NULL, NULL) > 0) {
            log_info("Found FPGA file at: %s", full_path_utf8);
            status = bladerf_load_fpga(dev, full_path_utf8);
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
#else
    char exe_path_buf[MAX_PATH_LEN] = {0};
    char exe_dir[MAX_PATH_LEN] = {0};
    char parent_dir_buf[MAX_PATH_LEN] = {0};
    
    ssize_t len = readlink("/proc/self/exe", exe_path_buf, sizeof(exe_path_buf) - 1);
    if (len > 0) {
        exe_path_buf[len] = '\0';
        char temp_path1[MAX_PATH_LEN];
        snprintf(temp_path1, sizeof(temp_path1), "%s", exe_path_buf);
        snprintf(exe_dir, sizeof(exe_dir), "%s", dirname(temp_path1));
        char temp_path2[MAX_PATH_LEN];
        snprintf(temp_path2, sizeof(temp_path2), "%s", exe_path_buf);
        dirname(temp_path2);
        snprintf(parent_dir_buf, sizeof(parent_dir_buf), "%s", dirname(temp_path2));
    } else {
        snprintf(exe_dir, sizeof(exe_dir), ".");
        snprintf(parent_dir_buf, sizeof(parent_dir_buf), "..");
    }

    const char* search_bases[] = {
        exe_dir,
        parent_dir_buf,
        "/usr/local/share/" APP_NAME,
        "/usr/share/" APP_NAME,
        NULL
    };
    char full_path[MAX_PATH_LEN];

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
