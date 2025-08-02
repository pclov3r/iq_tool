#define _POSIX_C_SOURCE 200809L

#include "setup.h"
#include "types.h"
#include "config.h"
#include "platform.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "log.h"
#include "input_source.h"
#include "file_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <liquid.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <liquid/liquid.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#endif

/**
 * @brief Resolves input and output paths.
 */
bool resolve_file_paths(AppConfig *config) {
    if (!config) return false;
#ifdef _WIN32
    if (config->input_filename_arg) {
        if (!get_absolute_path_windows(config->input_filename_arg,
                                       &config->effective_input_filename_w,
                                       &config->effective_input_filename_utf8)) {
            return false;
        }
    }
    if (config->output_filename_arg) {
        if (!get_absolute_path_windows(config->output_filename_arg,
                                       &config->effective_output_filename_w,
                                       &config->effective_output_filename_utf8)) {
             free_absolute_path_windows(&config->effective_input_filename_w, &config->effective_input_filename_utf8);
             return false;
        }
    }
#else
    config->effective_input_filename = config->input_filename_arg;
    config->effective_output_filename = config->output_filename_arg;
#endif
    return true;
}

/**
 * @brief Calculates and validates the resampling ratio.
 */
bool calculate_and_validate_resample_ratio(AppConfig *config, AppResources *resources, float *out_ratio) {
     if (!config || !resources || !out_ratio) return false;

    // --- ADDED: Handle --no-resample (passthrough) mode ---
    if (config->no_resample) {
        log_info("Passthrough mode enabled: output rate will match input rate.");
        config->target_rate = (double)resources->source_info.samplerate;
        resources->is_passthrough = true;
    } else {
        resources->is_passthrough = false;
    }
    // ------------------------------------------------------

    double input_rate_d = (double)resources->source_info.samplerate;
    float r = (float)(config->target_rate / input_rate_d);

    if (!isfinite(r) || r < MIN_ACCEPTABLE_RATIO || r > MAX_ACCEPTABLE_RATIO) {
        log_fatal("Error: Calculated resampling ratio (%.6f) is invalid or outside acceptable range.", r);
        return false;
    }
    *out_ratio = r;
    return true;
}

/**
 * @brief Allocates all necessary processing buffer pools.
 */
bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    double estimated_output = ceil((double)BUFFER_SIZE_SAMPLES * (double)resample_ratio);
    resources->max_out_samples = (unsigned int)estimated_output + 128;

    double upper_limit = 20.0 * (double)BUFFER_SIZE_SAMPLES * fmax(1.0, (double)resample_ratio);
    if (resources->max_out_samples > upper_limit || resources->max_out_samples > (UINT_MAX - 128)) {
         log_fatal("Error: Calculated output buffer size per item (%u samples) is unreasonable.", resources->max_out_samples);
         return false;
    }

    size_t raw_input_bytes_per_item = BUFFER_SIZE_SAMPLES * 2 * resources->input_bytes_per_sample;
    size_t complex_elements_per_input_item = BUFFER_SIZE_SAMPLES;
    size_t complex_elements_per_output_item = resources->max_out_samples;
    bool shift_requested = config->freq_shift_requested || config->set_center_frequency_target_hz;
    size_t complex_elements_per_shifted_item = shift_requested ? (config->shift_after_resample ? complex_elements_per_output_item : complex_elements_per_input_item) : 0;
    
    resources->output_bytes_per_sample_pair = (config->sample_format == SAMPLE_TYPE_CU8 || config->sample_format == SAMPLE_TYPE_CS8) ? (sizeof(uint8_t) * 2) : (sizeof(int16_t) * 2);
    size_t output_buffer_bytes_per_item = complex_elements_per_output_item * resources->output_bytes_per_sample_pair;

    resources->work_item_pool = malloc(NUM_BUFFERS * sizeof(WorkItem));
    resources->raw_input_pool = malloc(NUM_BUFFERS * raw_input_bytes_per_item);
    resources->complex_scaled_pool = malloc(NUM_BUFFERS * complex_elements_per_input_item * sizeof(complex_float_t));
    if (shift_requested) {
        resources->complex_shifted_pool = malloc(NUM_BUFFERS * complex_elements_per_shifted_item * sizeof(complex_float_t));
    }
    resources->complex_resampled_pool = malloc(NUM_BUFFERS * complex_elements_per_output_item * sizeof(complex_float_t));
    resources->output_pool = malloc(NUM_BUFFERS * output_buffer_bytes_per_item);

    if (!resources->work_item_pool || !resources->raw_input_pool || !resources->complex_scaled_pool ||
        (shift_requested && !resources->complex_shifted_pool) || !resources->complex_resampled_pool || !resources->output_pool) {
        log_fatal("Error: Failed to allocate one or more processing buffer pools.");
        return false;
    }

    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        WorkItem* item = &resources->work_item_pool[i];
        item->raw_input_buffer = (char*)resources->raw_input_pool + i * raw_input_bytes_per_item;
        item->complex_buffer_scaled = resources->complex_scaled_pool + i * complex_elements_per_input_item;
        item->complex_buffer_resampled = resources->complex_resampled_pool + i * complex_elements_per_output_item;
        item->output_buffer = (unsigned char*)resources->output_pool + i * output_buffer_bytes_per_item;
        if (shift_requested) {
             item->complex_buffer_shifted = resources->complex_shifted_pool + i * complex_elements_per_shifted_item;
        } else {
             item->complex_buffer_shifted = NULL;
        }
    }

    return true;
}

/**
 * @brief Creates and initializes the liquid-dsp components.
 */
bool create_dsp_components(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    if (!shift_create_nco(config, resources)) {
        return false;
    }

    // --- MODIFIED: Skip creating the resampler in passthrough mode ---
    if (!resources->is_passthrough) {
        resources->resampler = msresamp_crcf_create(resample_ratio, STOPBAND_ATTENUATION_DB);
        if (!resources->resampler) {
            log_fatal("Error: Failed to create liquid-dsp resampler object.");
            shift_destroy_nco(resources);
            return false;
        }
    }
    // -----------------------------------------------------------------

    return true;
}

/**
 * @brief Creates the thread-safe queues and mutexes.
 */
bool create_threading_components(AppResources *resources) {
    resources->free_pool_q = queue_create(NUM_BUFFERS);
    resources->input_q = queue_create(NUM_BUFFERS);
    resources->output_q = queue_create(NUM_BUFFERS);
    if (!resources->free_pool_q || !resources->input_q || !resources->output_q) {
        log_fatal("Failed to create one or more processing queues.");
        return false;
    }

    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        if (!queue_enqueue(resources->free_pool_q, &resources->work_item_pool[i])) {
             log_fatal("Failed to initially populate free item queue.");
             return false;
        }
    }

    if (pthread_mutex_init(&resources->progress_mutex, NULL) != 0) {
        log_fatal("Failed to initialize progress mutex: %s", strerror(errno));
        return false;
    }
    if (pthread_mutex_init(&resources->dsp_mutex, NULL) != 0) {
        log_fatal("Failed to initialize DSP mutex: %s", strerror(errno));
        return false;
    }
    return true;
}

/**
 * @brief Destroys the thread-safe queues and mutexes.
 */
void destroy_threading_components(AppResources *resources) {
    if(resources->input_q) queue_destroy(resources->input_q);
    if(resources->output_q) queue_destroy(resources->output_q);
    if(resources->free_pool_q) queue_destroy(resources->free_pool_q);

    pthread_mutex_destroy(&resources->dsp_mutex);
    pthread_mutex_destroy(&resources->progress_mutex);
}

/**
 * @brief Prints the full configuration summary to stderr.
 */
void print_configuration_summary(const AppConfig *config, const AppResources *resources) {
    if (!config || !resources || !resources->selected_input_ops) return;

    InputSummaryInfo summary_info;
    memset(&summary_info, 0, sizeof(InputSummaryInfo));

    const InputSourceContext ctx = { .config = config, .resources = (AppResources*)resources };
    resources->selected_input_ops->get_summary_info(&ctx, &summary_info);

    int max_label_len = 0;

    if (summary_info.count > 0) {
        for (int i = 0; i < summary_info.count; i++) {
            int len = (int)strlen(summary_info.items[i].label);
            if (len > max_label_len) {
                max_label_len = len;
            }
        }
    }

    const char* base_output_labels[] = { "Output Type", "Sample Type", "Output Rate", "Scale Factor", "Frequency Shift", "Output Target", "Resampling" };
    for (size_t i = 0; i < sizeof(base_output_labels) / sizeof(base_output_labels[0]); i++) {
        int len = (int)strlen(base_output_labels[i]);
        if (len > max_label_len) {
            max_label_len = len;
        }
    }
    if (config->set_center_frequency_target_hz) {
        int len = (int)strlen("Target Frequency");
        if (len > max_label_len) {
            max_label_len = len;
        }
    }

    fprintf(stderr, "--- Input Details ---\n");
    if (summary_info.count > 0) {
        for (int i = 0; i < summary_info.count; i++) {
            fprintf(stderr, "  %-*s : %s\n", max_label_len, summary_info.items[i].label, summary_info.items[i].value);
        }
    }

    fprintf(stderr, "--- Output Details ---\n");
    const char* output_type_str;
    switch (config->output_type) {
        case OUTPUT_TYPE_RAW:      output_type_str = "Raw"; break;
        case OUTPUT_TYPE_WAV:      output_type_str = "WAV"; break;
        case OUTPUT_TYPE_WAV_RF64: output_type_str = "WAV (RF64)"; break;
        default:                   output_type_str = "Unknown"; break;
    }
    fprintf(stderr, "  %-*s : %s\n", max_label_len, "Output Type", output_type_str);

    const char* sample_type_str;
    switch (config->sample_format) {
        case SAMPLE_TYPE_CU8:  sample_type_str = "cu8 (Unsigned 8-bit Complex)"; break;
        case SAMPLE_TYPE_CS8:  sample_type_str = "cs8 (Signed 8-bit Complex)";   break;
        case SAMPLE_TYPE_CS16: sample_type_str = "cs16 (Signed 16-bit Complex)"; break;
        default:               sample_type_str = "Unknown";                      break;
    }
    fprintf(stderr, "  %-*s : %s\n", max_label_len, "Sample Type", sample_type_str);
    fprintf(stderr, "  %-*s : %.1f Hz\n", max_label_len, "Output Rate", config->target_rate);
    
    // --- ADDED: Show resampling status ---
    if (resources->is_passthrough) {
        fprintf(stderr, "  %-*s : %s\n", max_label_len, "Resampling", "Disabled (Passthrough Mode)");
    }
    // -------------------------------------

    fprintf(stderr, "  %-*s : %.5f\n", max_label_len, "Scale Factor", config->scale_value);
    if (config->set_center_frequency_target_hz) {
        fprintf(stderr, "  %-*s : %.6f MHz\n", max_label_len, "Target Frequency", config->center_frequency_target_hz / 1e6);
    }
    char shift_buf[64];
    snprintf(shift_buf, sizeof(shift_buf), "%+.2f Hz%s",
             resources->actual_nco_shift_hz,
             config->shift_after_resample ? " (Post-Resample)" : "");
    fprintf(stderr, "  %-*s : %s\n", max_label_len, "Frequency Shift", shift_buf);

    const char* output_path_for_messages;
#ifdef _WIN32
    output_path_for_messages = config->effective_output_filename_utf8;
#else
    output_path_for_messages = config->effective_output_filename;
#endif
    fprintf(stderr, "  %-*s : %s\n", max_label_len, "Output Target", config->output_to_stdout ? "<stdout>" : output_path_for_messages);
}

/**
 * @brief Checks for Nyquist warning and prompts user if necessary.
 */
bool check_nyquist_warning(const AppConfig *config, const AppResources *resources) {
    return shift_check_nyquist_warning(config, resources);
}

/**
 * @brief Prepares the output stream (file or stdout).
 */
bool prepare_output_stream(AppConfig *config, AppResources *resources) {
     if (!config || !resources) return false;

    if (!file_writer_init(&resources->writer_ctx, config)) {
        return false;
    }

    if (!resources->writer_ctx.ops.open(&resources->writer_ctx, config, resources)) {
        return false;
    }

    return true;
}
