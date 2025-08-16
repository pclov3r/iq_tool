#include "input_hackrf.h"
#include "config.h"
#include "types.h"
#include "signal_handler.h"
#include "log.h"
#include "spectrum_shift.h"
#include "utils.h"
#include "sample_convert.h"
#include "input_common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include "argparse.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

extern pthread_mutex_t g_console_mutex;

extern AppConfig g_config;

void hackrf_set_default_config(AppConfig* config) {
    config->hackrf.lna_gain = HACKRF_DEFAULT_LNA_GAIN;
    config->hackrf.hackrf_lna_gain_arg = HACKRF_DEFAULT_LNA_GAIN;
    config->hackrf.vga_gain = HACKRF_DEFAULT_VGA_GAIN;
    config->hackrf.hackrf_vga_gain_arg = HACKRF_DEFAULT_VGA_GAIN;
    config->hackrf.sample_rate_hz = HACKRF_DEFAULT_SAMPLE_RATE;
}

static const struct argparse_option hackrf_cli_options[] = {
    OPT_GROUP("HackRF-Specific Options"),
    OPT_FLOAT(0, "hackrf-sample-rate", &g_config.hackrf.hackrf_sample_rate_hz_arg, "Set sample rate in Hz. (Optional, Default: 8e6)", NULL, 0, 0),
    OPT_INTEGER(0, "hackrf-lna-gain", &g_config.hackrf.hackrf_lna_gain_arg, "Set LNA (IF) gain in dB. (Optional, Default: 16)", NULL, 0, 0),
    OPT_INTEGER(0, "hackrf-vga-gain", &g_config.hackrf.hackrf_vga_gain_arg, "Set VGA (Baseband) gain in dB. (Optional, Default: 0)", NULL, 0, 0),
    OPT_BOOLEAN(0, "hackrf-amp-enable", &g_config.hackrf.amp_enable, "Enable the front-end RF amplifier (+14 dB).", NULL, 0, 0),
};

const struct argparse_option* hackrf_get_cli_options(int* count) {
    *count = sizeof(hackrf_cli_options) / sizeof(hackrf_cli_options[0]);
    return hackrf_cli_options;
}

static bool hackrf_initialize(InputSourceContext* ctx);
static void* hackrf_start_stream(InputSourceContext* ctx);
static void hackrf_stop_stream(InputSourceContext* ctx);
static void hackrf_cleanup(InputSourceContext* ctx);
static void hackrf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool hackrf_validate_options(AppConfig* config);

static InputSourceOps hackrf_ops = {
    .initialize = hackrf_initialize,
    .start_stream = hackrf_start_stream,
    .stop_stream = hackrf_stop_stream,
    .cleanup = hackrf_cleanup,
    .get_summary_info = hackrf_get_summary_info,
    .validate_options = hackrf_validate_options,
    .has_known_length = _input_source_has_known_length_false
};

InputSourceOps* get_hackrf_input_ops(void) {
    return &hackrf_ops;
}

static bool hackrf_validate_options(AppConfig* config) {
    if (config->hackrf.hackrf_lna_gain_arg != HACKRF_DEFAULT_LNA_GAIN) {
        long lna_gain = config->hackrf.hackrf_lna_gain_arg;
        if (lna_gain < 0 || lna_gain > 40 || (lna_gain % 8 != 0)) {
            log_fatal("Invalid LNA gain %ld dB. Must be 0-40 in 8 dB steps.", lna_gain);
            return false;
        }
        config->hackrf.lna_gain = (uint32_t)lna_gain;
        config->hackrf.lna_gain_provided = true;
    }

    if (config->hackrf.hackrf_vga_gain_arg != HACKRF_DEFAULT_VGA_GAIN) {
        long vga_gain = config->hackrf.hackrf_vga_gain_arg;
        if (vga_gain < 0 || vga_gain > 62 || (vga_gain % 2 != 0)) {
            log_fatal("Invalid VGA gain %ld dB. Must be 0-62 in 2 dB steps.", vga_gain);
            return false;
        }
        config->hackrf.vga_gain = (uint32_t)vga_gain;
        config->hackrf.vga_gain_provided = true;
    }

    if (config->hackrf.hackrf_sample_rate_hz_arg != 0.0f) {
        config->hackrf.sample_rate_hz = (double)config->hackrf.hackrf_sample_rate_hz_arg;
        if (config->hackrf.sample_rate_hz < 2e6 || config->hackrf.sample_rate_hz > 20e6) {
            log_fatal("Invalid HackRF sample rate %.0f Hz. Must be between 2,000,000 and 20,000,000.", config->hackrf.sample_rate_hz);
            return false;
        }
        config->hackrf.sample_rate_provided = true;
    }

    return true;
}

int hackrf_stream_callback(hackrf_transfer* transfer) {
    AppResources *resources = (AppResources*)transfer->rx_ctx;
    const AppConfig *config = resources->config;

    if (is_shutdown_requested() || resources->error_occurred) {
        return -1;
    }

    if (transfer->valid_length == 0) {
        return 0;
    }

    // --- START OF MODIFIED BLOCK ---
    if (config->raw_passthrough) {
        size_t bytes_written = file_write_buffer_write(resources->file_write_buffer, transfer->buffer, transfer->valid_length);
        if (bytes_written < (size_t)transfer->valid_length) {
            log_warn("I/O buffer overrun! Dropped %zu bytes.", (size_t)transfer->valid_length - bytes_written);
        }
        return 0;
    }
    // --- END OF MODIFIED BLOCK ---

    // Normal processing path
    size_t bytes_processed = 0;
    while (bytes_processed < (size_t)transfer->valid_length) {
        SampleChunk *item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!item) {
            log_warn("Processor queue full. Dropping %zu bytes.", (size_t)transfer->valid_length - bytes_processed);
            return 0;
        }

        item->stream_discontinuity_event = false;

        size_t chunk_size = transfer->valid_length - bytes_processed;
        const size_t pipeline_buffer_size = BUFFER_SIZE_SAMPLES * resources->input_bytes_per_sample_pair;
        if (chunk_size > pipeline_buffer_size) {
            chunk_size = pipeline_buffer_size;
        }

        memcpy(item->raw_input_data, transfer->buffer + bytes_processed, chunk_size);
        item->frames_read = chunk_size / resources->input_bytes_per_sample_pair;
        item->is_last_chunk = false;

        if (item->frames_read > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_frames_read += item->frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);
        }

        if (!queue_enqueue(resources->raw_to_pre_process_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            return -1;
        }
        bytes_processed += chunk_size;
    }
    return 0;
}

static void hackrf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;
    add_summary_item(info, "Input Source", "HackRF One");
    add_summary_item(info, "Input Format", "8-bit Signed Complex (cs8)");
    add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);
    add_summary_item(info, "Gain", "LNA: %u dB, VGA: %u dB", config->hackrf.lna_gain, config->hackrf.vga_gain);
    add_summary_item(info, "RF Amp", "%s", config->hackrf.amp_enable ? "Enabled" : "Disabled");
    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
}

static bool hackrf_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    int result;

    result = hackrf_init();
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_init() failed: %s (%d)", hackrf_error_name(result), result);
        return false;
    }

    result = hackrf_open(&resources->hackrf_dev);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_open() failed: %s (%d)", hackrf_error_name(result), result);
        hackrf_exit();
        return false;
    }
    log_info("Found HackRF One.");

    double sample_rate_to_set;
    if (config->hackrf.sample_rate_provided) {
        sample_rate_to_set = config->hackrf.sample_rate_hz;
    } else {
        sample_rate_to_set = HACKRF_DEFAULT_SAMPLE_RATE;
    }

    result = hackrf_set_sample_rate(resources->hackrf_dev, sample_rate_to_set);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_sample_rate() failed: %s (%d)", hackrf_error_name(result), result);
        hackrf_close(resources->hackrf_dev); resources->hackrf_dev = NULL; hackrf_exit(); return false;
    }

    result = hackrf_set_freq(resources->hackrf_dev, (uint64_t)config->sdr.rf_freq_hz);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_freq() failed: %s (%d)", hackrf_error_name(result), result);
        hackrf_close(resources->hackrf_dev); resources->hackrf_dev = NULL; hackrf_exit(); return false;
    }

    result = hackrf_set_lna_gain(resources->hackrf_dev, config->hackrf.lna_gain);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_lna_gain() failed: %s (%d)", hackrf_error_name(result), result);
        hackrf_close(resources->hackrf_dev); resources->hackrf_dev = NULL; hackrf_exit(); return false;
    }

    result = hackrf_set_vga_gain(resources->hackrf_dev, config->hackrf.vga_gain);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_vga_gain() failed: %s (%d)", hackrf_error_name(result), result);
        hackrf_close(resources->hackrf_dev); resources->hackrf_dev = NULL; hackrf_exit(); return false;
    }

    result = hackrf_set_amp_enable(resources->hackrf_dev, (uint8_t)config->hackrf.amp_enable);
    if (result != HACKRF_SUCCESS) {
        log_fatal("hackrf_set_amp_enable() failed: %s (%d)", hackrf_error_name(result), result);
        hackrf_close(resources->hackrf_dev); resources->hackrf_dev = NULL; hackrf_exit(); return false;
    }

    if (config->sdr.bias_t_enable) {
        result = hackrf_set_antenna_enable(resources->hackrf_dev, 1);
        if (result != HACKRF_SUCCESS) {
            log_fatal("hackrf_set_antenna_enable() failed: %s (%d)", hackrf_error_name(result), result);
            hackrf_close(resources->hackrf_dev); resources->hackrf_dev = NULL; hackrf_exit(); return false;
        }
    }

    resources->input_format = CS8;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    resources->source_info.samplerate = (int)sample_rate_to_set;
    resources->source_info.frames = -1;

    if (config->raw_passthrough && resources->input_format != config->output_format) {
        log_fatal("Option --raw-passthrough requires input and output formats to be identical. HackRF input is 'cs8', but output was set to '%s'.", config->sample_type_name);
        hackrf_close(resources->hackrf_dev);
        resources->hackrf_dev = NULL;
        hackrf_exit();
        return false;
    }

    return true;
}

static void* hackrf_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    int result;

    log_info("Starting HackRF stream...");
    result = hackrf_start_rx(resources->hackrf_dev, hackrf_stream_callback, resources);
    if (result != HACKRF_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hackrf_start_rx() failed: %s (%d)", hackrf_error_name(result), result);
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

static void hackrf_stop_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->hackrf_dev && hackrf_is_streaming(resources->hackrf_dev) == HACKRF_TRUE) {
        log_info("Stopping HackRF stream...");
        int result = hackrf_stop_rx(resources->hackrf_dev);
        if (result != HACKRF_SUCCESS) {
            log_error("Failed to stop HackRF RX: %s (%d)", hackrf_error_name(result), result);
        }
    }
}

static void hackrf_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->hackrf_dev) {
        log_info("Closing HackRF device...");
        hackrf_close(resources->hackrf_dev);
        resources->hackrf_dev = NULL;
    }
    log_info("Exiting HackRF library...");
    hackrf_exit();
}
