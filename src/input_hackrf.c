#include "input_hackrf.h" // Its own header, which now includes input_source.h
#include "config.h"         // For HACKRF_DEFAULT_SAMPLE_RATE, BUFFER_SIZE_SAMPLES
#include "types.h"          // For AppResources, AppConfig, WorkItem, Queue
#include "signal_handler.h" // For is_shutdown_requested
#include "log.h"            // For logging
#include "spectrum_shift.h" // For shift_reset_nco in callbacks
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h> // For va_list, va_start, va_end

#ifdef _WIN32
#include <windows.h> // For Sleep()
#else
#include <unistd.h>  // For nanosleep()
#include <time.h>    // For nanosleep()
#endif

// Add an external declaration for the global console mutex defined in main.c
extern pthread_mutex_t g_console_mutex;

// --- Helper Function ---

static void handle_fatal_sdr_error(const char* error_msg, AppResources *resources) {
    pthread_mutex_lock(&resources->progress_mutex);
    if (resources->error_occurred) {
        pthread_mutex_unlock(&resources->progress_mutex);
        return;
    }
    resources->error_occurred = true;
    pthread_mutex_unlock(&resources->progress_mutex);

    log_fatal("%s", error_msg);
    log_info("Signaling all threads to shut down...");
    if (resources->input_q) queue_signal_shutdown(resources->input_q);
    if (resources->output_q) queue_signal_shutdown(resources->output_q);
    if (resources->free_pool_q) queue_signal_shutdown(resources->free_pool_q);
}

// --- HackRF API Callback Function (remains public as it's called by libhackrf) ---

int hackrf_stream_callback(hackrf_transfer* transfer) {
    AppResources *resources = (AppResources*)transfer->rx_ctx;

    if (is_shutdown_requested() || resources->error_occurred) {
        return -1; // Signal to libhackrf to stop streaming
    }

    if (transfer->valid_length == 0) {
        return 0; // Continue streaming
    }

    size_t bytes_processed = 0;
    while (bytes_processed < (size_t)transfer->valid_length) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->free_pool_q);
        if (!item) {
            log_warn("Processor queue full. Dropping %zu bytes.", (size_t)transfer->valid_length - bytes_processed);
            return 0;
        }

        size_t chunk_size = transfer->valid_length - bytes_processed;
        size_t pipeline_buffer_size = BUFFER_SIZE_SAMPLES * 2;

        if (chunk_size > pipeline_buffer_size) {
            chunk_size = pipeline_buffer_size;
        }

        memcpy(item->raw_input_buffer, transfer->buffer + bytes_processed, chunk_size);
        
        item->frames_read = chunk_size / 2;
        item->is_last_chunk = false;

        if (item->frames_read > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_frames_read += item->frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);
        }

        if (!queue_enqueue(resources->input_q, item)) {
            queue_enqueue(resources->free_pool_q, item);
            return -1;
        }

        bytes_processed += chunk_size;
    }

    return 0;
}

// --- InputSourceOps Implementations for HackRF ---

/**
 * @brief A helper to safely add a new key-value pair to the summary info struct.
 */
static void add_summary_item(InputSummaryInfo* info, const char* label, const char* value_fmt, ...) {
    if (info->count >= MAX_SUMMARY_ITEMS) {
        return; // Prevent buffer overflow
    }
    SummaryItem* item = &info->items[info->count];
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->label[sizeof(item->label) - 1] = '\0';

    va_list args;
    va_start(args, value_fmt);
    vsnprintf(item->value, sizeof(item->value), value_fmt, args);
    va_end(args);
    item->value[sizeof(item->value) - 1] = '\0';

    info->count++;
}

/**
 * @brief Populates the InputSummaryInfo struct with details for a HackRF source.
 */
static void hackrf_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;

    add_summary_item(info, "Input Source", "HackRF One");
    add_summary_item(info, "Input Format", "8-bit Signed PCM");
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
    return true; // HackRF is SDR hardware
}


/**
 * @brief Initializes the HackRF device and library.
 */
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
        // --- THIS LINE IS THE FIX ---
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

    resources->input_bit_depth = 8;
    resources->input_bytes_per_sample = sizeof(int8_t);
    resources->source_info.samplerate = (int)sample_rate_to_set;
    resources->source_info.frames = -1;
    resources->input_pcm_format = PCM_FORMAT_S8;

    return true;
}

/**
 * @brief Starts the HackRF stream. This function runs in the reader thread.
 */
static void* hackrf_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    int result;

    log_info("Starting HackRF stream...");
    result = hackrf_start_rx(resources->hackrf_dev, hackrf_stream_callback, resources);
    if (result != HACKRF_SUCCESS) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "hackrf_start_rx() failed: %s (%d)", hackrf_error_name(result), result);
        handle_fatal_sdr_error(error_buf, resources);
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

/**
 * @brief Stops the HackRF stream.
 */
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

/**
 * @brief Cleans up HackRF resources.
 */
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

// The single instance of InputSourceOps for HackRF
static InputSourceOps hackrf_ops = {
    .initialize = hackrf_initialize,
    .start_stream = hackrf_start_stream,
    .stop_stream = hackrf_stop_stream,
    .cleanup = hackrf_cleanup,
    .get_summary_info = hackrf_get_summary_info,
    .validate_options = hackrf_validate_options,
    .is_sdr_hardware = hackrf_is_sdr_hardware
};

/**
 * @brief Public function to get the HackRF input source operations.
 */
InputSourceOps* get_hackrf_input_ops(void) {
    return &hackrf_ops;
}
