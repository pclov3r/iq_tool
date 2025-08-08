#include "input_hackrf.h"
#include "config.h"
#include "types.h"
#include "signal_handler.h"
#include "log.h"
#include "spectrum_shift.h"
#include "utils.h"
#include "sample_convert.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

extern pthread_mutex_t g_console_mutex;


// --- HackRF API Callback Function ---

int hackrf_stream_callback(hackrf_transfer* transfer) {
    AppResources *resources = (AppResources*)transfer->rx_ctx;

    if (is_shutdown_requested() || resources->error_occurred) {
        return -1;
    }

    if (transfer->valid_length == 0) {
        return 0;
    }

    size_t bytes_processed = 0;
    while (bytes_processed < (size_t)transfer->valid_length) {
        SampleChunk *item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!item) {
            log_warn("Processor queue full. Dropping %zu bytes.", (size_t)transfer->valid_length - bytes_processed);
            return 0;
        }

        // *** FIX: Explicitly initialize flags for this SampleChunk ***
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


// --- InputSourceOps Implementations for HackRF ---

static void hackrf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;
    add_summary_item(info, "Input Source", "HackRF One");
    add_summary_item(info, "Input Format", "8-bit Signed Complex (cs8)");
    add_summary_item(info, "Input Rate", "%.3f Msps", (double)resources->source_info.samplerate / 1e6);
    add_summary_item(info, "RF Frequency", "%.6f MHz", config->sdr.rf_freq_hz / 1e6);
    add_summary_item(info, "Gain", "LNA: %u dB, VGA: %u dB", config->hackrf.lna_gain, config->hackrf.vga_gain);
    add_summary_item(info, "RF Amp", "%s", config->hackrf.amp_enable ? "Enabled" : "Disabled");
    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
}

static bool hackrf_validate_options(const AppConfig* config) {
    if (!config->sdr.rf_freq_provided) {
        log_fatal("Option --rf-freq <hz> is required when using an SDR input.");
        return false;
    }
    if (config->set_center_frequency_target_hz || config->freq_shift_requested) {
        log_fatal("Frequency shifting options (--target-freq, --shift-freq) are only valid for 'wav' input.");
        return false;
    }
    if (config->hackrf.lna_gain_provided) {
        if (config->hackrf.lna_gain > 40 || (config->hackrf.lna_gain % 8 != 0)) {
            log_fatal("Invalid LNA gain %u dB. Must be 0-40 in 8 dB steps.", config->hackrf.lna_gain);
            return false;
        }
    }
    if (config->hackrf.vga_gain_provided) {
        if (config->hackrf.vga_gain > 62 || (config->hackrf.vga_gain % 2 != 0)) {
            log_fatal("Invalid VGA gain %u dB. Must be 0-62 in 2 dB steps.", config->hackrf.vga_gain);
            return false;
        }
    }
    if (config->hackrf.sample_rate_provided) {
        if (config->hackrf.sample_rate_hz < 2e6 || config->hackrf.sample_rate_hz > 20e6) {
            log_fatal("Invalid HackRF sample rate %.0f Hz. Must be between 2,000,000 and 20,000,000.", config->hackrf.sample_rate_hz);
            return false;
        }
    }
    return true;
}

static bool hackrf_is_sdr_hardware(void) {
    return true;
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

static InputSourceOps hackrf_ops = {
    .initialize = hackrf_initialize,
    .start_stream = hackrf_start_stream,
    .stop_stream = hackrf_stop_stream,
    .cleanup = hackrf_cleanup,
    .get_summary_info = hackrf_get_summary_info,
    .validate_options = hackrf_validate_options,
    .is_sdr_hardware = hackrf_is_sdr_hardware
};

InputSourceOps* get_hackrf_input_ops(void) {
    return &hackrf_ops;
}
