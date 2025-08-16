#include "input_rtlsdr.h"
#include "log.h"
#include "signal_handler.h"
#include "config.h"
#include "types.h"
#include "utils.h"
#include "sample_convert.h"
#include "input_common.h"
#include <string.h>
#include <errno.h>
#include "argparse.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

extern AppConfig g_config;

void rtlsdr_set_default_config(AppConfig* config) {
    config->rtlsdr.sample_rate_hz = RTLSDR_DEFAULT_SAMPLE_RATE;
}

static const struct argparse_option rtlsdr_cli_options[] = {
    OPT_GROUP("RTL-SDR-Specific Options"),
    OPT_INTEGER(0, "rtlsdr-device-idx", &g_config.rtlsdr.device_index, "Select specific RTL-SDR device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_FLOAT(0, "rtlsdr-sample-rate", &g_config.rtlsdr.sample_rate_hz, "Set sample rate in Hz. (Optional, Default: 2.4e6)", NULL, 0, 0),
    OPT_FLOAT(0, "rtlsdr-gain", &g_config.rtlsdr.rtlsdr_gain_db_arg, "Set manual tuner gain in dB (e.g., 28.0, 49.6). Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "rtlsdr-ppm", &g_config.rtlsdr.ppm, "Set frequency correction in parts-per-million. (Optional, Default: 0)", NULL, 0, 0),
    OPT_INTEGER(0, "rtlsdr-direct-sampling", &g_config.rtlsdr.direct_sampling_mode, "Enable direct sampling mode for HF reception (1=I-branch, 2=Q-branch)", NULL, 0, 0),
};

const struct argparse_option* rtlsdr_get_cli_options(int* count) {
    *count = sizeof(rtlsdr_cli_options) / sizeof(rtlsdr_cli_options[0]);
    return rtlsdr_cli_options;
}

static bool rtlsdr_initialize(InputSourceContext* ctx);
static void* rtlsdr_start_stream(InputSourceContext* ctx);
static void rtlsdr_stop_stream(InputSourceContext* ctx);
static void rtlsdr_cleanup(InputSourceContext* ctx);
static void rtlsdr_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool rtlsdr_validate_options(AppConfig* config);
static void rtlsdr_stream_callback(unsigned char *buf, uint32_t len, void *cb_ctx);

static const char* get_tuner_name_from_enum(enum rtlsdr_tuner tuner_type) {
    switch (tuner_type) {
        case RTLSDR_TUNER_E4000:    return "Elonics E4000";
        case RTLSDR_TUNER_FC0012:   return "Fitipower FC0012";
        case RTLSDR_TUNER_FC0013:   return "Fitipower FC0013";
        case RTLSDR_TUNER_FC2580:   return "Fitipower FC2580";
        case RTLSDR_TUNER_R820T:    return "Rafael Micro R820T";
        case RTLSDR_TUNER_R828D:    return "Rafael Micro R828D";
        case RTLSDR_TUNER_UNKNOWN:
        default:                    return "Unknown Tuner";
    }
}

static InputSourceOps rtlsdr_ops = {
    .initialize = rtlsdr_initialize,
    .start_stream = rtlsdr_start_stream,
    .stop_stream = rtlsdr_stop_stream,
    .cleanup = rtlsdr_cleanup,
    .get_summary_info = rtlsdr_get_summary_info,
    .validate_options = rtlsdr_validate_options,
    .has_known_length = _input_source_has_known_length_false
};

InputSourceOps* get_rtlsdr_input_ops(void) {
    return &rtlsdr_ops;
}

static bool rtlsdr_validate_options(AppConfig* config) {
    if (config->rtlsdr.rtlsdr_gain_db_arg != 0.0f) {
        config->rtlsdr.gain = (int)(config->rtlsdr.rtlsdr_gain_db_arg * 10.0f);
        config->rtlsdr.gain_provided = true;
    }

    if (config->rtlsdr.sample_rate_hz != RTLSDR_DEFAULT_SAMPLE_RATE) {
        config->rtlsdr.sample_rate_provided = true;
    }

    if (config->rtlsdr.ppm != 0) {
        config->rtlsdr.ppm_provided = true;
    }

    if (config->rtlsdr.direct_sampling_mode != 0) {
        if (config->rtlsdr.direct_sampling_mode < 1 || config->rtlsdr.direct_sampling_mode > 2) {
            log_fatal("Invalid value for --rtlsdr-direct-sampling. Must be 1 or 2.");
            return false;
        }
        config->rtlsdr.direct_sampling_provided = true;
    }

    return true;
}

static void rtlsdr_stream_callback(unsigned char *buf, uint32_t len, void *cb_ctx) {
    AppResources *resources = (AppResources*)cb_ctx;
    const AppConfig *config = resources->config;

    if (is_shutdown_requested() || resources->error_occurred) {
        return;
    }

    if (len == 0) {
        return;
    }

    if (config->raw_passthrough) {
        // --- START OF MODIFIED BLOCK (PASSTHROUGH) ---
        size_t bytes_written = file_write_buffer_write(resources->file_write_buffer, buf, len);
        if (bytes_written < len) {
            log_warn("I/O buffer overrun! Dropped %zu bytes.", len - bytes_written);
        }
        // In passthrough mode, we don't use the SampleChunk pool, so we just return.
        return;
        // --- END OF MODIFIED BLOCK (PASSTHROUGH) ---
    }

    // Normal processing path
    SampleChunk *item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
    if (!item) {
        return; // No free buffers, so we must drop the data.
    }

    item->stream_discontinuity_event = false;

    size_t bytes_to_copy = len;
    const size_t pipeline_buffer_size = BUFFER_SIZE_SAMPLES * resources->input_bytes_per_sample_pair;
    if (bytes_to_copy > pipeline_buffer_size) {
        log_warn("RTL-SDR callback provided more samples than buffer can hold. Truncating.");
        bytes_to_copy = pipeline_buffer_size;
    }

    memcpy(item->raw_input_data, buf, bytes_to_copy);
    item->frames_read = bytes_to_copy / resources->input_bytes_per_sample_pair;
    item->is_last_chunk = false;

    if (item->frames_read > 0) {
        pthread_mutex_lock(&resources->progress_mutex);
        resources->total_frames_read += item->frames_read;
        pthread_mutex_unlock(&resources->progress_mutex);
    }

    if (!queue_enqueue(resources->raw_to_pre_process_queue, item)) {
        queue_enqueue(resources->free_sample_chunk_queue, item);
    }
}

static bool rtlsdr_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    int result;
    uint32_t device_count;
    uint32_t device_index = config->rtlsdr.device_index;

    log_info("Attempting to initialize RTL-SDR device...");

    device_count = rtlsdr_get_device_count();
    if (device_count == 0) {
        log_fatal("No RTL-SDR devices found.");
        return false;
    }
    log_info("Found %d RTL-SDR device(s).", device_count);

    if (device_index >= device_count) {
        log_fatal("Device index %u is out of range. Found %u devices.", device_index, device_count);
        return false;
    }

    log_info("Reading device information for index %d...", device_index);
    if (rtlsdr_get_device_usb_strings(device_index, resources->rtlsdr_manufact, resources->rtlsdr_product, resources->rtlsdr_serial) < 0) {
        log_fatal("Failed to read USB device strings for device %d.", device_index);
        return false;
    }

    log_info("Opening device %d: %s %s, S/N: %s", device_index, resources->rtlsdr_manufact, resources->rtlsdr_product, resources->rtlsdr_serial);
    result = rtlsdr_open(&resources->rtlsdr_dev, device_index);
    if (result < 0) {
        log_fatal("Failed to open RTL-SDR device: %s", strerror(-result));
        return false;
    }
    
    enum rtlsdr_tuner tuner_type = rtlsdr_get_tuner_type(resources->rtlsdr_dev);
    const char* tuner_name = get_tuner_name_from_enum(tuner_type);
    log_info("Found RTL-SDR device with tuner: %s", tuner_name);

    double sample_rate_to_set = config->rtlsdr.sample_rate_provided ? config->rtlsdr.sample_rate_hz : RTLSDR_DEFAULT_SAMPLE_RATE;
    result = rtlsdr_set_sample_rate(resources->rtlsdr_dev, (uint32_t)sample_rate_to_set);
    if (result < 0) {
        log_fatal("Failed to set sample rate: %s", strerror(-result));
        rtlsdr_close(resources->rtlsdr_dev);
        return false;
    }
    uint32_t actual_rate = rtlsdr_get_sample_rate(resources->rtlsdr_dev);
    log_info("RTL-SDR: Requested sample rate %.0f Hz, actual rate set to %u Hz.", sample_rate_to_set, actual_rate);
    resources->source_info.samplerate = actual_rate;

    result = rtlsdr_set_center_freq(resources->rtlsdr_dev, (uint32_t)config->sdr.rf_freq_hz);
    if (result < 0) {
        log_fatal("Failed to set center frequency: %s", strerror(-result));
        rtlsdr_close(resources->rtlsdr_dev);
        return false;
    }

    if (config->rtlsdr.gain_provided) {
        rtlsdr_set_tuner_gain_mode(resources->rtlsdr_dev, 1);
        rtlsdr_set_tuner_gain(resources->rtlsdr_dev, config->rtlsdr.gain);
    } else {
        rtlsdr_set_tuner_gain_mode(resources->rtlsdr_dev, 0);
    }
    
    if (config->rtlsdr.ppm_provided) {
        rtlsdr_set_freq_correction(resources->rtlsdr_dev, config->rtlsdr.ppm);
    }

    if (config->sdr.bias_t_enable) {
        log_info("Attempting to enable Bias-T...");
        result = rtlsdr_set_bias_tee(resources->rtlsdr_dev, 1);
        if (result != 0) {
            log_warn("Failed to enable Bias-T. The device may not support this feature.");
        }
    }
    
    if (config->rtlsdr.direct_sampling_provided) {
        rtlsdr_set_direct_sampling(resources->rtlsdr_dev, config->rtlsdr.direct_sampling_mode);
    }

    result = rtlsdr_reset_buffer(resources->rtlsdr_dev);
    if (result < 0) {
        log_warn("Failed to reset RTL-SDR buffer.");
    }

    resources->input_format = CU8;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    resources->source_info.frames = -1;

    if (config->raw_passthrough && resources->input_format != config->output_format) {
        log_fatal("Option --raw-passthrough requires input and output formats to be identical. RTL-SDR input is 'cu8', but output was set to '%s'.", config->sample_type_name);
        rtlsdr_close(resources->rtlsdr_dev);
        return false;
    }

    return true;
}

static void* rtlsdr_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    int result;

    log_info("Starting RTL-SDR stream...");
    result = rtlsdr_read_async(resources->rtlsdr_dev, rtlsdr_stream_callback, resources, 0, 0);
    if (result < 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "rtlsdr_read_async() failed: %s", strerror(-result));
        handle_fatal_thread_error(error_buf, resources);
        return NULL;
    }

    while (!is_shutdown_requested() && !resources->error_occurred) {
#ifdef _WIN32
        Sleep(100);
#else
        struct timespec sleep_time = {0, 100000000L};
        nanosleep(&sleep_time, NULL);
#endif
    }
    return NULL;
}

static void rtlsdr_stop_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->rtlsdr_dev) {
        log_info("Stopping RTL-SDR stream...");
        rtlsdr_cancel_async(resources->rtlsdr_dev);
    }
}

static void rtlsdr_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->rtlsdr_dev) {
        log_info("Closing RTL-SDR device...");
        rtlsdr_close(resources->rtlsdr_dev);
        resources->rtlsdr_dev = NULL;
    }
}

static void rtlsdr_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    
    char source_name_buf[775];
    snprintf(source_name_buf, sizeof(source_name_buf), "%s %s (S/N: %s)", 
             resources->rtlsdr_manufact, 
             resources->rtlsdr_product, 
             resources->rtlsdr_serial);

    add_summary_item(info, "Input Source", "%s", source_name_buf);
    add_summary_item(info, "Input Format", "8-bit Unsigned Complex (cu8)");
    add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);
    if (config->rtlsdr.gain_provided) {
        add_summary_item(info, "Gain", "%.1f dB (Manual)", (float)config->rtlsdr.gain / 10.0f);
    } else {
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }
    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
    if (config->rtlsdr.ppm_provided) {
        add_summary_item(info, "PPM Correction", "%d", config->rtlsdr.ppm);
    }
}
