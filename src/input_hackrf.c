#include "input_hackrf.h"
#include "module.h"
#include "constants.h"
#include "app_context.h"
#include "signal_handler.h"
#include "log.h"
#include "frequency_shift.h"
#include "utils.h"
#include "sample_convert.h"
#include "input_common.h"
#include "memory_arena.h"
#include "queue.h"
#include "sdr_packet_serializer.h"
#include "argparse.h"
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

extern AppConfig g_config;

// --- Private Module Configuration ---
static struct {
    uint32_t lna_gain;
    bool lna_gain_provided;
    long hackrf_lna_gain_arg;
    uint32_t vga_gain;
    bool vga_gain_provided;
    long hackrf_vga_gain_arg;
    bool amp_enable;
} s_hackrf_config;

// --- Private Module State ---
typedef struct {
    hackrf_device* dev;
} HackrfPrivateData;


void hackrf_set_default_config(AppConfig* config) {
    config->sdr.sample_rate_hz = HACKRF_DEFAULT_SAMPLE_RATE;
    s_hackrf_config.lna_gain = HACKRF_DEFAULT_LNA_GAIN;
    s_hackrf_config.hackrf_lna_gain_arg = HACKRF_DEFAULT_LNA_GAIN;
    s_hackrf_config.vga_gain = HACKRF_DEFAULT_VGA_GAIN;
    s_hackrf_config.hackrf_vga_gain_arg = HACKRF_DEFAULT_VGA_GAIN;
}

static const struct argparse_option hackrf_cli_options[] = {
    OPT_GROUP("HackRF-Specific Options"),
    OPT_INTEGER(0, "hackrf-lna-gain", &s_hackrf_config.hackrf_lna_gain_arg, "Set LNA (IF) gain in dB. (Optional, Default: 16)", NULL, 0, 0),
    OPT_INTEGER(0, "hackrf-vga-gain", &s_hackrf_config.hackrf_vga_gain_arg, "Set VGA (Baseband) gain in dB. (Optional, Default: 0)", NULL, 0, 0),
    OPT_BOOLEAN(0, "hackrf-amp-enable", &s_hackrf_config.amp_enable, "Enable the front-end RF amplifier (+14 dB).", NULL, 0, 0),
};

const struct argparse_option* hackrf_get_cli_options(int* count) {
    *count = sizeof(hackrf_cli_options) / sizeof(hackrf_cli_options[0]);
    return hackrf_cli_options;
}

static bool hackrf_initialize(ModuleContext* ctx);
static void* hackrf_start_stream(ModuleContext* ctx);
static void hackrf_stop_stream(ModuleContext* ctx);
static void hackrf_cleanup(ModuleContext* ctx);
static void hackrf_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool hackrf_validate_options(AppConfig* config);
static bool hackrf_validate_generic_options(const AppConfig* config);
static int hackrf_realtime_stream_callback(hackrf_transfer* transfer);
static int hackrf_buffered_stream_callback(hackrf_transfer* transfer);


static ModuleInterface hackrf_module_api = {
    .initialize = hackrf_initialize,
    .start_stream = hackrf_start_stream,
    .stop_stream = hackrf_stop_stream,
    .cleanup = hackrf_cleanup,
    .get_summary_info = hackrf_get_summary_info,
    .validate_options = hackrf_validate_options,
    .validate_generic_options = hackrf_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

ModuleInterface* get_hackrf_input_module_api(void) {
    return &hackrf_module_api;
}

static bool hackrf_validate_generic_options(const AppConfig* config) {
    if (!config->sdr.rf_freq_provided) {
        log_fatal("HackRF input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool hackrf_validate_options(AppConfig* config) {
    if (s_hackrf_config.hackrf_lna_gain_arg != HACKRF_DEFAULT_LNA_GAIN) {
        long lna_gain = s_hackrf_config.hackrf_lna_gain_arg;
        if (lna_gain < 0 || lna_gain > 40 || (lna_gain % 8 != 0)) {
            log_fatal("Invalid LNA gain %ld dB. Must be 0-40 in 8 dB steps.", lna_gain);
            return false;
        }
        s_hackrf_config.lna_gain = (uint32_t)lna_gain;
        s_hackrf_config.lna_gain_provided = true;
    }

    if (s_hackrf_config.hackrf_vga_gain_arg != HACKRF_DEFAULT_VGA_GAIN) {
        long vga_gain = s_hackrf_config.hackrf_vga_gain_arg;
        if (vga_gain < 0 || vga_gain > 62 || (vga_gain % 2 != 0)) {
            log_fatal("Invalid VGA gain %ld dB. Must be 0-62 in 2 dB steps.", vga_gain);
            return false;
        }
        s_hackrf_config.vga_gain = (uint32_t)vga_gain;
        s_hackrf_config.vga_gain_provided = true;
    }

    if (config->sdr.sample_rate_provided) {
        if (config->sdr.sample_rate_hz < 2e6 || config->sdr.sample_rate_hz > 20e6) {
            log_fatal("Invalid HackRF sample rate %.0f Hz. Must be between 2,000,000 and 20,000,000.", config->sdr.sample_rate_hz);
            return false;
        }
    }

    return true;
}

static int hackrf_buffered_stream_callback(hackrf_transfer* transfer) {
    AppResources *resources = (AppResources*)transfer->rx_ctx;

    // --- HEARTBEAT ---
    sdr_input_update_heartbeat(resources);

    if (is_shutdown_requested() || resources->error_occurred) {
        return -1;
    }

    // Simply hand off the entire buffer to the reusable chunker.
    sdr_write_interleaved_chunks(
        resources,
        transfer->buffer,
        transfer->valid_length,
        resources->input_bytes_per_sample_pair,
        CS8
    );

    return 0;
}

static int hackrf_realtime_stream_callback(hackrf_transfer* transfer) {
    AppResources *resources = (AppResources*)transfer->rx_ctx;
    const AppConfig *config = resources->config;

    // --- HEARTBEAT ---
    sdr_input_update_heartbeat(resources);

    if (is_shutdown_requested() || resources->error_occurred) {
        return -1;
    }

    if (transfer->valid_length == 0) {
        return 0;
    }

    if (config->raw_passthrough) {
        size_t written = resources->writer_ctx.api.write(&resources->writer_ctx, transfer->buffer, transfer->valid_length);
        if (written < (size_t)transfer->valid_length) {
            log_debug("Real-time passthrough: stdout write error, consumer likely closed pipe.");
            request_shutdown();
            return -1;
        }
        return 0;
    }

    size_t bytes_processed = 0;
    while (bytes_processed < (size_t)transfer->valid_length) {
        SampleChunk *item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!item) {
            log_warn("Real-time pipeline stalled. Dropping %zu bytes.", (size_t)transfer->valid_length - bytes_processed);
            return 0;
        }

        item->stream_discontinuity_event = false;

        size_t chunk_size = transfer->valid_length - bytes_processed;
        const size_t pipeline_buffer_size = PIPELINE_CHUNK_BASE_SAMPLES * resources->input_bytes_per_sample_pair;
        if (chunk_size > pipeline_buffer_size) {
            chunk_size = pipeline_buffer_size;
        }

        memcpy(item->raw_input_data, transfer->buffer + bytes_processed, chunk_size);
        item->frames_read = chunk_size / resources->input_bytes_per_sample_pair;
        item->is_last_chunk = false;
	    item->packet_sample_format = resources->input_format;

        if (item->frames_read > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_frames_read += item->frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);
        }

        if (!queue_enqueue(resources->reader_output_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            return -1;
        }
        bytes_processed += chunk_size;
    }
    return 0;
}


static void hackrf_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;
    add_summary_item(info, "Input Source", "HackRF One");
    add_summary_item(info, "Input Format", "8-bit Signed Complex (cs8)");
    add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);

    // as HackRF does not have a true hardware AGC. The gain is always fixed.
    add_summary_item(info, "LNA Gain", "%u dB", s_hackrf_config.lna_gain);
    add_summary_item(info, "VGA Gain", "%u dB", s_hackrf_config.vga_gain);
    add_summary_item(info, "RF Amp", "%s", s_hackrf_config.amp_enable ? "Enabled" : "Disabled");
    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
}

static bool hackrf_initialize(ModuleContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    int result;
    bool success = false; // Assume failure until the very end

    HackrfPrivateData* private_data = (HackrfPrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(HackrfPrivateData), true);
    if (!private_data) {
        return false; // mem_arena_alloc logs error, no resources to clean up yet
    }
    private_data->dev = NULL; // Initialize resource state
    resources->input_module_private_data = private_data;

    result = hackrf_init();
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_init() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup; // On error, jump to the single cleanup point
    }

    result = hackrf_open(&private_data->dev);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_open() failed: %s (%d)", hackrf_error_name(result), result);
        private_data->dev = NULL; // Ensure dev is NULL on failure
        goto cleanup;
    }
    log_info("Found HackRF One.");

    result = hackrf_set_sample_rate(private_data->dev, config->sdr.sample_rate_hz);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_sample_rate() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_freq(private_data->dev, (uint64_t)config->sdr.rf_freq_hz);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_freq() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_lna_gain(private_data->dev, s_hackrf_config.lna_gain);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_lna_gain() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_vga_gain(private_data->dev, s_hackrf_config.vga_gain);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_vga_gain() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    result = hackrf_set_amp_enable(private_data->dev, (uint8_t)s_hackrf_config.amp_enable);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_amp_enable() failed: %s (%d)", hackrf_error_name(result), result);
        goto cleanup;
    }

    if (config->sdr.bias_t_enable) {
        result = hackrf_set_antenna_enable(private_data->dev, 1);
        if (result != HACKRF_SUCCESS) {
            log_fatal("hackrf_set_antenna_enable() failed: %s (%d)", hackrf_error_name(result), result);
            goto cleanup;
        }
    }

    resources->input_format = CS8;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    resources->source_info.samplerate = (int)config->sdr.sample_rate_hz;
    resources->source_info.frames = -1;

    if (config->raw_passthrough && resources->input_format != config->output_format) {
        log_fatal("Option --raw-passthrough requires input and output formats to be identical. HackRF input is 'cs8', but output was set to '%s'.", config->sample_type_name);
        goto cleanup;
    }

    success = true;

cleanup:
    if (!success) {
        // The main application cleanup will call hackrf_cleanup(), which handles these.
    }
    return success;
}

static void* hackrf_start_stream(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    HackrfPrivateData* private_data = (HackrfPrivateData*)resources->input_module_private_data;
    int result;
    hackrf_sample_block_cb_fn callback_fn;

    if (resources->pipeline_mode == PIPELINE_MODE_BUFFERED_SDR) {
        log_info("Starting HackRF stream (Buffered Mode)...");
        callback_fn = hackrf_buffered_stream_callback;
    } else { // PIPELINE_MODE_REALTIME_SDR
        log_info("Starting HackRF stream (Real-Time Mode)...");
        callback_fn = hackrf_realtime_stream_callback;
    }

    result = hackrf_start_rx(private_data->dev, callback_fn, resources);
    if (result != HACKRF_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hackrf_start_rx() failed: %s (%d)", hackrf_error_name(result), result);
        handle_fatal_thread_error(error_buf, resources);
        return NULL;
    }

    while (!is_shutdown_requested() && !resources->error_occurred && hackrf_is_streaming(private_data->dev) == HACKRF_TRUE) {
#ifdef _WIN32
        Sleep(100);
#else
        struct timespec sleep_time = {0, 100000000L};
        nanosleep(&sleep_time, NULL);
#endif
    }

    hackrf_stop_stream(ctx);

    return NULL;
}

static void hackrf_stop_stream(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    HackrfPrivateData* private_data = (HackrfPrivateData*)resources->input_module_private_data;
    if (private_data && private_data->dev && hackrf_is_streaming(private_data->dev) == HACKRF_TRUE) {
        log_info("Stopping HackRF stream...");
        int result = hackrf_stop_rx(private_data->dev);
        if (result != HACKRF_SUCCESS) {
            log_error("Failed to stop HackRF RX: %s (%d)", hackrf_error_name(result), result);
        }
    }
}

static void hackrf_cleanup(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->input_module_private_data) {
        HackrfPrivateData* private_data = (HackrfPrivateData*)resources->input_module_private_data;
        if (private_data->dev) {
            log_info("Closing HackRF device...");
            hackrf_close(private_data->dev);
            private_data->dev = NULL;
        }
        resources->input_module_private_data = NULL;
    }
    log_info("Exiting HackRF library...");
    hackrf_exit();
}
