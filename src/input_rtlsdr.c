/**
 * @file input_rtlsdr.c
 */

#include "input_rtlsdr.h"
#include "module.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "app_context.h"
#include "utilities.h"
#include "sample_format_table.h"
#include "input_common.h"
#include "mem_arena.h"
#include "argparse.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Module-specific includes
#include <rtl-sdr.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#include <strings.h>
#endif

// --- Private Module Configuration ---
static struct {
    int device_index;
    int gain;
    bool gain_provided;
    float rtlsdr_gain_db_arg;
    int ppm;
    bool ppm_provided;
    int direct_sampling_mode;
    bool direct_sampling_provided;
} s_rtlsdr_config;

// --- Private Module State ---
typedef struct {
    rtlsdr_dev_t *dev;
    char manufact[256];
    char product[256];
    char serial[256];
    unsigned char *passthrough_buffer;
    pthread_mutex_t driver_mutex;
} RtlSdrContext;

void rtlsdr_set_default_config(AppConfig* config) {
    config->sdr_general.sample_rate_hz = RTLSDR_DEFAULT_SAMPLE_RATE;
}

static const struct argparse_option rtlsdr_input_cli_options[] = {
    OPT_GROUP("RTL-SDR Input (rtlsdr)"),
    OPT_INTEGER(0, "rtlsdr-device-index", &s_rtlsdr_config.device_index, "Select specific RTL-SDR device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_FLOAT(0, "rtlsdr-gain", &s_rtlsdr_config.rtlsdr_gain_db_arg, "Set manual tuner gain in dB (e.g., 28.0, 49.6). Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "rtlsdr-ppm", &s_rtlsdr_config.ppm, "Set frequency correction in parts-per-million. (Optional, Default: 0)", NULL, 0, 0),
    OPT_INTEGER(0, "rtlsdr-direct-sampling", &s_rtlsdr_config.direct_sampling_mode, "Enable direct sampling mode for HF reception (1=I-branch, 2=Q-branch)", NULL, 0, 0),
};

const struct argparse_option* rtlsdr_input_get_cli_options(int* count) {
    *count = sizeof(rtlsdr_input_cli_options) / sizeof(rtlsdr_input_cli_options[0]);
    return rtlsdr_input_cli_options;
}

static void rtlsdr_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info);
static bool rtlsdr_input_validate_options(AppContext* app);
static bool rtlsdr_input_validate_generic_options(const AppConfig* config);

static int rtlsdr_find_nearest_gain(rtlsdr_dev_t *dev,
                                    int requested_gain_tenths,
                                    MemoryArena* arena)
{
    int n = rtlsdr_get_tuner_gains(dev, NULL);
    if (n <= 0)
        return requested_gain_tenths;

    // Allocate gains[] array from the memory arena
    int *gains = mem_arena_alloc(arena, sizeof(int) * n, false);
    if (!gains) {
        // Arena exhausted → fallback to no snapping
        return requested_gain_tenths;
    }

    rtlsdr_get_tuner_gains(dev, gains);

    int best_gain  = gains[0];
    int best_delta = abs(requested_gain_tenths - best_gain);

    for (int i = 1; i < n; i++) {
        int delta = abs(requested_gain_tenths - gains[i]);
        if (delta < best_delta) {
            best_delta = delta;
            best_gain  = gains[i];
        }
    }

    return best_gain;
}

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

static bool rtlsdr_input_validate_generic_options(const AppConfig* config) {
    if (!config->sdr_general.rf_freq_provided) {
        log_error("RTL-SDR input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool rtlsdr_input_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    if (s_rtlsdr_config.rtlsdr_gain_db_arg != 0.0f) {
        s_rtlsdr_config.gain = (int)(s_rtlsdr_config.rtlsdr_gain_db_arg * 10.0f);
        s_rtlsdr_config.gain_provided = true;
    }

    if (s_rtlsdr_config.ppm != 0) {
        s_rtlsdr_config.ppm_provided = true;
    }

    if (s_rtlsdr_config.direct_sampling_mode != 0) {
        if (s_rtlsdr_config.direct_sampling_mode < 1 || s_rtlsdr_config.direct_sampling_mode > 2) {
            log_error("Invalid value for --rtlsdr-direct-sampling. Must be 1 or 2.");
            return false;
        }
        s_rtlsdr_config.direct_sampling_provided = true;
    }

    if (config->sdr_general.sample_rate_provided) {
        if (config->sdr_general.sample_rate_hz < 225001 || config->sdr_general.sample_rate_hz > 3200000) {
             log_error("Invalid sample rate for RTL-SDR: %.15g Hz. Must be between 225001 and 3200000.", config->sdr_general.sample_rate_hz);
             return false;
        }
    }

    return true;
}

static void rtlsdr_input_stream_callback(unsigned char *buffer, uint32_t length, void *cb_context) {
    AppContext* app = (AppContext*)cb_context;

    // --- HEARTBEAT ---

    if (is_shutdown_requested() || app->stats.error_occurred) {
        return;
    }

    // --- NEW ARCHITECTURE: DUMP THE WHOLE BLOCK ---
    // RTL-SDR provides interleaved CU8 (1 byte I, 1 byte Q = 2 bytes per sample).
    // We no longer chop this up. We send the full hardware buffer to the ring buffer.
    uint32_t num_samples = length / 2;

    if (!app->module.queue_samples(app->module.pipeline_context, buffer, num_samples, CU8)) {
        /* Warning handled internally by pipeline */
    }
}

static bool rtlsdr_input_initialize(ModuleContext* context) {
    const AppConfig *config = context->config;
    AppContext* app = context->app;
    int result;
    uint32_t device_count;
    uint32_t device_index = s_rtlsdr_config.device_index;
    bool success = false; // Assume failure until the very end

    RtlSdrContext* private_data = (RtlSdrContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(RtlSdrContext), true);
    if (!private_data) {
        return false;
    }

    // Allocate persistent buffer for realtime passthrough (16KB)
    private_data->passthrough_buffer = (unsigned char*)mem_arena_alloc(&app->pipeline.setup_arena, RTLSDR_PASSTHROUGH_BUFFER_SIZE, false);

    if (pthread_mutex_init(&private_data->driver_mutex, NULL) != 0) {
        log_error("Failed to init driver mutex.");
        return false;
    }
    if (!private_data->passthrough_buffer) {
        return false;
    }

    private_data->dev = NULL; // Initialize resource state
    app->module.input_private_data = private_data;

    device_count = rtlsdr_get_device_count();
    if (device_count == 0) {
        log_error("No RTL-SDR devices found.");
        goto cleanup;
    }
    log_info("Found %d RTL-SDR device(s).", device_count);

    if (device_index >= device_count) {
        log_error("Device index %u is out of range. Found %u devices.", device_index, device_count);
        goto cleanup;
    }
    if (rtlsdr_get_device_usb_strings(device_index, private_data->manufact, private_data->product, private_data->serial) < 0) {
        log_fatal("Failed to read USB device strings for device %d.", device_index);
        goto cleanup;
    }

    log_info("Opening device %d: %s %s, S/N: %s", device_index, private_data->manufact, private_data->product, private_data->serial);
    result = rtlsdr_open(&private_data->dev, device_index);
    if (result < 0) {
        log_fatal("Failed to open RTL-SDR device: %s", strerror(-result));
        private_data->dev = NULL; // Ensure dev is NULL on failure
        goto cleanup;
    }

    enum rtlsdr_tuner tuner_type = rtlsdr_get_tuner_type(private_data->dev);
    const char* tuner_name = get_tuner_name_from_enum(tuner_type);
    log_info("Found RTL-SDR device with tuner: %s", tuner_name);

    result = rtlsdr_set_sample_rate(private_data->dev, (uint32_t)config->sdr_general.sample_rate_hz);
    if (result < 0) {
        log_fatal("Failed to set sample rate: %s", strerror(-result));
        goto cleanup;
    }
    uint32_t actual_rate = rtlsdr_get_sample_rate(private_data->dev);
    log_info("RTL-SDR: Requested sample rate %.15g Hz, actual rate set to %.15g Hz.", config->sdr_general.sample_rate_hz, (double)actual_rate);
    app->module.source_info.sample_rate = actual_rate;

    result = rtlsdr_set_center_freq(private_data->dev, (uint32_t)config->sdr_general.rf_freq_hz);
    if (result < 0) {
        log_fatal("Failed to set center frequency: %s", strerror(-result));
        goto cleanup;
    }

    if (s_rtlsdr_config.gain_provided) {
        rtlsdr_set_tuner_gain_mode(private_data->dev, 1);

        int requested = s_rtlsdr_config.gain;
        int nearest = rtlsdr_find_nearest_gain(
            private_data->dev,
            requested,
            &app->pipeline.setup_arena
        );

        result = rtlsdr_set_tuner_gain(private_data->dev, nearest);
        if (result < 0) {
            log_fatal("Failed to set tuner gain to %.1f dB.", nearest / 10.0f);
            goto cleanup;
        }

        int actual = rtlsdr_get_tuner_gain(private_data->dev);

        log_info(
            "Requested gain: %.1f dB -> Actual gain set: %.1f dB",
            requested / 10.0f,
            actual    / 10.0f
        );
    } else {
        rtlsdr_set_tuner_gain_mode(private_data->dev, 0);
    }

    int resolved_ppm = 0;
    bool ppm_from_serial = false;

    if (s_rtlsdr_config.ppm_provided) {
        resolved_ppm = s_rtlsdr_config.ppm;
        log_info("RTL-SDR: Using manually provided PPM correction: %d", resolved_ppm);
    } else {
        int temp_ppm = 0;
        // Parse "PPMxxxx" or "ppmxxxx" format from the serial string
        if (sscanf(private_data->serial, "%*3[PpMm]%d", &temp_ppm) == 1) {
            resolved_ppm = temp_ppm;
            ppm_from_serial = true;
            log_info("RTL-SDR: Auto-detected PPM correction from serial: %d", resolved_ppm);
        }
    }

    if (resolved_ppm != 0) {
        result = rtlsdr_set_freq_correction(private_data->dev, resolved_ppm);
        if (result < 0) {
            if (ppm_from_serial) {
                log_warn("RTL-SDR: Failed to set automatic PPM correction from serial (%d): %s", resolved_ppm, strerror(-result));
            } else {
                log_warn("RTL-SDR: Failed to set manual PPM correction (%d): %s", resolved_ppm, strerror(-result));
            }
        }
    }

    if (config->sdr_general.bias_t_enable) {
        result = rtlsdr_set_bias_tee(private_data->dev, 1);
        if (result != 0) {
            log_warn("Failed to enable Bias-T. The device may not support this feature.");
        }
    }

    if (s_rtlsdr_config.direct_sampling_provided) {
        rtlsdr_set_direct_sampling(private_data->dev, s_rtlsdr_config.direct_sampling_mode);
    }

    result = rtlsdr_reset_buffer(private_data->dev);
    if (result < 0) {
        log_warn("Failed to reset RTL-SDR buffer.");
    }

    app->module.input_format = CU8;
    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(app->module.input_format);
    app->module.source_info.frames = -1;

    // Force the pipeline into BUFFERED_INPUT mode.
    // This ensures we use the Async callback (rtlsdr_read_async) and the large RingBuffer.
    // This decouples the USB read timing from the output pipe backpressure, preventing
    // sample drops when piping to downstream tools.

    success = true; // All steps succeeded

cleanup:
    if (!success) {
        // If we failed, the main cleanup routine will be called.
    }
    return success;
}

static void* rtlsdr_input_push_samples_to_queue(ModuleContext* context, QueueSamples queue_samples, void* pipeline_context) {
    context->app->module.queue_samples = queue_samples;
    context->app->module.pipeline_context = pipeline_context;
    AppContext* app = context->app;
    RtlSdrContext* private_data = (RtlSdrContext*)app->module.input_private_data;
    int result;

    // NOTE: rtlsdr_read_async BLOCKS until the stream stops or is cancelled.
    result = rtlsdr_read_async(private_data->dev, rtlsdr_input_stream_callback, app, 0, 0);

    if (result < 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "rtlsdr_read_async() failed: %s", strerror(-result));
        handle_fatal_thread_error(error_buf, app);
        return NULL;
    }

    return NULL;
}

static void rtlsdr_input_stop_sample_queue_push(ModuleContext* context) {
    AppContext* app = context->app;
    RtlSdrContext* private_data = (RtlSdrContext*)app->module.input_private_data;
    if (private_data && private_data->dev) {
        log_debug("Stopping RTL-SDR stream...");
        rtlsdr_cancel_async(private_data->dev);
    }
}

static void rtlsdr_input_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (app->module.input_private_data) {
        RtlSdrContext* private_data = (RtlSdrContext*)app->module.input_private_data;
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->dev) {
            // Reset buffer to clear USB stalls before closing; prevents I2C errors
            rtlsdr_reset_buffer(private_data->dev);
            rtlsdr_close(private_data->dev);
            private_data->dev = NULL;
        }
        pthread_mutex_unlock(&private_data->driver_mutex);
        pthread_mutex_destroy(&private_data->driver_mutex);
        app->module.input_private_data = NULL;
    }
}

static void rtlsdr_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info) {
    const AppConfig *config = context->config;
    AppContext* app = context->app;
    RtlSdrContext* private_data = (RtlSdrContext*)app->module.input_private_data;

    char source_name_buf[775];
    snprintf(source_name_buf, sizeof(source_name_buf), "%s %s (S/N: %s)",
             private_data->manufact,
             private_data->product,
             private_data->serial);

    add_summary_item(info, "Input Source", "%s", source_name_buf);
    add_summary_item(info, "Input Format", "8-bit Unsigned Complex (cu8)");
    add_summary_item(info, "Input Sample Rate", "%.15g Hz", (double)app->module.source_info.sample_rate);
    if (s_rtlsdr_config.gain_provided) {
        add_summary_item(info, "Gain", "%.1f dB (Manual)", (float)s_rtlsdr_config.gain / 10.0f);
    } else {
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }
    add_summary_item(info, "Bias-T", "%s", config->sdr_general.bias_t_enable ? "Enabled" : "Disabled");
    if (s_rtlsdr_config.ppm_provided) {
        add_summary_item(info, "PPM Correction", "%d", s_rtlsdr_config.ppm);
    }
}

// --- The InputModuleInterface V-Table ---
static InputModuleInterface s_rtlsdr_input_api = {
    .initialize = rtlsdr_input_initialize,
    .push_samples_to_queue = rtlsdr_input_push_samples_to_queue,
    .stop_sample_queue_push = rtlsdr_input_stop_sample_queue_push,
    .cleanup = rtlsdr_input_cleanup,
    .get_summary_info = rtlsdr_input_get_summary_info,
    .validate_options = rtlsdr_input_validate_options,
    .validate_generic_options = rtlsdr_input_validate_generic_options,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_rtlsdr_get_module_api(void) {
    return &s_rtlsdr_input_api;
}
