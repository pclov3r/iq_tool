#include "input_rtlsdr.h"
#include "input_source.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "app_context.h"
#include "utils.h"
#include "sample_convert.h"
#include "input_common.h"
#include "memory_arena.h"
#include "queue.h"
#include "sdr_packet_serializer.h"
#include "argparse.h"
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
} RtlSdrPrivateData;

extern AppConfig g_config;

void rtlsdr_set_default_config(AppConfig* config) {
    config->sdr.sample_rate_hz = RTLSDR_DEFAULT_SAMPLE_RATE;
}

static const struct argparse_option rtlsdr_cli_options[] = {
    OPT_GROUP("RTL-SDR-Specific Options"),
    OPT_INTEGER(0, "rtlsdr-device-idx", &s_rtlsdr_config.device_index, "Select specific RTL-SDR device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_FLOAT(0, "rtlsdr-gain", &s_rtlsdr_config.rtlsdr_gain_db_arg, "Set manual tuner gain in dB (e.g., 28.0, 49.6). Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "rtlsdr-ppm", &s_rtlsdr_config.ppm, "Set frequency correction in parts-per-million. (Optional, Default: 0)", NULL, 0, 0),
    OPT_INTEGER(0, "rtlsdr-direct-sampling", &s_rtlsdr_config.direct_sampling_mode, "Enable direct sampling mode for HF reception (1=I-branch, 2=Q-branch)", NULL, 0, 0),
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
static bool rtlsdr_validate_generic_options(const AppConfig* config);
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
    .validate_generic_options = rtlsdr_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

InputSourceOps* get_rtlsdr_input_ops(void) {
    return &rtlsdr_ops;
}

static bool rtlsdr_validate_generic_options(const AppConfig* config) {
    if (!config->sdr.rf_freq_provided) {
        log_fatal("RTL-SDR input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool rtlsdr_validate_options(AppConfig* config) {
    if (s_rtlsdr_config.rtlsdr_gain_db_arg != 0.0f) {
        s_rtlsdr_config.gain = (int)(s_rtlsdr_config.rtlsdr_gain_db_arg * 10.0f);
        s_rtlsdr_config.gain_provided = true;
    }

    if (s_rtlsdr_config.ppm != 0) {
        s_rtlsdr_config.ppm_provided = true;
    }

    if (s_rtlsdr_config.direct_sampling_mode != 0) {
        if (s_rtlsdr_config.direct_sampling_mode < 1 || s_rtlsdr_config.direct_sampling_mode > 2) {
            log_fatal("Invalid value for --rtlsdr-direct-sampling. Must be 1 or 2.");
            return false;
        }
        s_rtlsdr_config.direct_sampling_provided = true;
    }

    if (config->sdr.sample_rate_provided) {
        if (config->sdr.sample_rate_hz < 225001 || config->sdr.sample_rate_hz > 3200000) {
             log_fatal("Invalid sample rate for RTL-SDR: %.0f Hz. Must be between 225001 and 3200000.", config->sdr.sample_rate_hz);
             return false;
        }
    }

    return true;
}

static void rtlsdr_stream_callback(unsigned char *buf, uint32_t len, void *cb_ctx) {
    AppResources *resources = (AppResources*)cb_ctx;

    // --- HEARTBEAT ---
    sdr_input_update_heartbeat(resources);

    if (is_shutdown_requested() || resources->error_occurred) {
        return;
    }

    // Simply hand off the entire buffer to the reusable chunker.
    sdr_write_interleaved_chunks(
        resources,
        buf,
        len,
        resources->input_bytes_per_sample_pair,
        CU8
    );
}

static bool rtlsdr_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    int result;
    uint32_t device_count;
    uint32_t device_index = s_rtlsdr_config.device_index;
    bool success = false; // Assume failure until the very end

    log_info("Attempting to initialize RTL-SDR device...");

    RtlSdrPrivateData* private_data = (RtlSdrPrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(RtlSdrPrivateData), true);
    if (!private_data) {
        return false;
    }
    private_data->dev = NULL; // Initialize resource state
    resources->input_module_private_data = private_data;

    device_count = rtlsdr_get_device_count();
    if (device_count == 0) {
        log_fatal("No RTL-SDR devices found.");
        goto cleanup;
    }
    log_info("Found %d RTL-SDR device(s).", device_count);

    if (device_index >= device_count) {
        log_fatal("Device index %u is out of range. Found %u devices.", device_index, device_count);
        goto cleanup;
    }

    log_info("Reading device information for index %d...", device_index);
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

    result = rtlsdr_set_sample_rate(private_data->dev, (uint32_t)config->sdr.sample_rate_hz);
    if (result < 0) {
        log_fatal("Failed to set sample rate: %s", strerror(-result));
        goto cleanup;
    }
    uint32_t actual_rate = rtlsdr_get_sample_rate(private_data->dev);
    log_info("RTL-SDR: Requested sample rate %.0f Hz, actual rate set to %u Hz.", config->sdr.sample_rate_hz, actual_rate);
    resources->source_info.samplerate = actual_rate;

    result = rtlsdr_set_center_freq(private_data->dev, (uint32_t)config->sdr.rf_freq_hz);
    if (result < 0) {
        log_fatal("Failed to set center frequency: %s", strerror(-result));
        goto cleanup;
    }

    if (s_rtlsdr_config.gain_provided) {
        rtlsdr_set_tuner_gain_mode(private_data->dev, 1);
        rtlsdr_set_tuner_gain(private_data->dev, s_rtlsdr_config.gain);
    } else {
        rtlsdr_set_tuner_gain_mode(private_data->dev, 0);
    }
    
    if (s_rtlsdr_config.ppm_provided) {
        rtlsdr_set_freq_correction(private_data->dev, s_rtlsdr_config.ppm);
    }

    if (config->sdr.bias_t_enable) {
        log_info("Attempting to enable Bias-T...");
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

    resources->input_format = CU8;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    resources->source_info.frames = -1;

    if (config->raw_passthrough && resources->input_format != config->output_format) {
        log_fatal("Option --raw-passthrough requires input and output formats to be identical. RTL-SDR input is 'cu8', but output was set to '%s'.", config->sample_type_name);
        goto cleanup;
    }

    success = true; // All steps succeeded

cleanup:
    if (!success) {
        // If we failed, the main cleanup routine will be called.
    }
    return success;
}

static void* rtlsdr_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    const AppConfig *config = ctx->config;
    RtlSdrPrivateData* private_data = (RtlSdrPrivateData*)resources->input_module_private_data;
    int result;

    switch (resources->pipeline_mode) {
        case PIPELINE_MODE_BUFFERED_SDR:
            log_info("Starting RTL-SDR stream (Buffered Mode)...");
            result = rtlsdr_read_async(private_data->dev, rtlsdr_stream_callback, resources, 0, 0);
            if (result < 0) {
                char error_buf[256];
                snprintf(error_buf, sizeof(error_buf), "rtlsdr_read_async() failed: %s", strerror(-result));
                handle_fatal_thread_error(error_buf, resources);
                return NULL;
            }
            // In async mode, the main loop just waits for shutdown. The callback does the work.
            while (!is_shutdown_requested() && !resources->error_occurred) {
                #ifdef _WIN32
                Sleep(100);
                #else
                struct timespec sleep_time = {0, 100000000L};
                nanosleep(&sleep_time, NULL);
                #endif
            }
            break;

        case PIPELINE_MODE_REALTIME_SDR:
            log_info("Starting RTL-SDR stream (Real-Time Mode)...");
            if (config->raw_passthrough) {
                unsigned char passthrough_buffer[16384];
                while (!is_shutdown_requested() && !resources->error_occurred) {
                    int n_read = 0;
                    result = rtlsdr_read_sync(private_data->dev, passthrough_buffer, sizeof(passthrough_buffer), &n_read);
                    
                    if (result >= 0) {
                        // --- HEARTBEAT ---
                        sdr_input_update_heartbeat(resources);
                    }

                    if (result < 0) {
                        if (!is_shutdown_requested()) {
                            char error_buf[256];
                            snprintf(error_buf, sizeof(error_buf), "rtlsdr_read_sync() failed: %s", strerror(-result));
                            handle_fatal_thread_error(error_buf, resources);
                        }
                        break;
                    }
                    if (n_read > 0) {
                        size_t written = resources->writer_ctx.ops.write(&resources->writer_ctx, passthrough_buffer, n_read);
                        if (written < (size_t)n_read) {
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

                    int n_read = 0;
                    size_t bytes_to_read = PIPELINE_CHUNK_BASE_SAMPLES * resources->input_bytes_per_sample_pair;
                    result = rtlsdr_read_sync(private_data->dev, item->raw_input_data, bytes_to_read, &n_read);

                    if (result >= 0) {
                        // --- HEARTBEAT ---
                        sdr_input_update_heartbeat(resources);
                    }

                    if (result < 0) {
                        if (!is_shutdown_requested()) {
                            char error_buf[256];
                            snprintf(error_buf, sizeof(error_buf), "rtlsdr_read_sync() failed: %s", strerror(-result));
                            handle_fatal_thread_error(error_buf, resources);
                        }
                        queue_enqueue(resources->free_sample_chunk_queue, item);
                        break;
                    }

                    item->frames_read = n_read / resources->input_bytes_per_sample_pair;
                    item->is_last_chunk = false;
                    item->stream_discontinuity_event = false;
		            item->packet_sample_format = resources->input_format;

                    if (item->frames_read > 0) {
                        pthread_mutex_lock(&resources->progress_mutex);
                        resources->total_frames_read += item->frames_read;
                        pthread_mutex_unlock(&resources->progress_mutex);
                        if (!queue_enqueue(resources->reader_output_queue, item)) {
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
                    queue_enqueue(resources->reader_output_queue, last_item);
                }
            }
            break;
        
        case PIPELINE_MODE_FILE_PROCESSING:
            // This case is not applicable for SDRs, but included for completeness.
            break;
    }
    return NULL;
}

static void rtlsdr_stop_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    RtlSdrPrivateData* private_data = (RtlSdrPrivateData*)resources->input_module_private_data;
    if (private_data && private_data->dev) {
        log_info("Stopping RTL-SDR stream...");
        rtlsdr_cancel_async(private_data->dev);
    }
}

static void rtlsdr_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->input_module_private_data) {
        RtlSdrPrivateData* private_data = (RtlSdrPrivateData*)resources->input_module_private_data;
        if (private_data->dev) {
            log_info("Closing RTL-SDR device...");
            rtlsdr_close(private_data->dev);
            private_data->dev = NULL;
        }
        resources->input_module_private_data = NULL;
    }
}

static void rtlsdr_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    RtlSdrPrivateData* private_data = (RtlSdrPrivateData*)resources->input_module_private_data;
    
    char source_name_buf[775];
    snprintf(source_name_buf, sizeof(source_name_buf), "%s %s (S/N: %s)", 
             private_data->manufact, 
             private_data->product, 
             private_data->serial);

    add_summary_item(info, "Input Source", "%s", source_name_buf);
    add_summary_item(info, "Input Format", "8-bit Unsigned Complex (cu8)");
    add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);
    if (s_rtlsdr_config.gain_provided) {
        add_summary_item(info, "Gain", "%.1f dB (Manual)", (float)s_rtlsdr_config.gain / 10.0f);
    } else {
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }
    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
    if (s_rtlsdr_config.ppm_provided) {
        add_summary_item(info, "PPM Correction", "%d", s_rtlsdr_config.ppm);
    }
}
