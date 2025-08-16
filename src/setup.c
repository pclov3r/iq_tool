// src/setup.c

#include "setup.h"
#include "types.h"
#include "config.h"
#include "platform.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "log.h"
#include "input_source.h"
#include "file_writer.h"
#include "sample_convert.h"
#include "iq_correct.h"
#include "dc_block.h"
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

bool resolve_file_paths(AppConfig *config) {
    if (!config) return false;

#ifdef _WIN32
    if (config->input_filename_arg) {
        if (!get_absolute_path_windows(config->input_filename_arg, &config->effective_input_filename_w, &config->effective_input_filename_utf8)) {
            return false;
        }
    }
    if (config->output_filename_arg) {
        if (!get_absolute_path_windows(config->output_filename_arg, &config->effective_output_filename_w, &config->effective_output_filename_utf8)) {
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

bool calculate_and_validate_resample_ratio(AppConfig *config, AppResources *resources, float *out_ratio) {
    if (!config || !resources || !out_ratio) return false;

    if (config->no_resample || config->raw_passthrough) {
        if (config->raw_passthrough) {
            log_info("Raw Passthrough mode enabled: Bypassing all DSP blocks.");
        } else {
            log_info("Native rate processing enabled: output rate will match input rate.");
        }
        config->target_rate = (double)resources->source_info.samplerate;
        resources->is_passthrough = true;
    } else {
        resources->is_passthrough = false;
    }

    double input_rate_d = (double)resources->source_info.samplerate;
    float r = (float)(config->target_rate / input_rate_d);

    if (!isfinite(r) || r < MIN_ACCEPTABLE_RATIO || r > MAX_ACCEPTABLE_RATIO) {
        log_fatal("Error: Calculated resampling ratio (%.6f) is invalid or outside acceptable range.", r);
        return false;
    }
    *out_ratio = r;

    if (resources->source_info.frames > 0) {
        resources->expected_total_output_frames = (long long)round((double)resources->source_info.frames * (double)r);
    } else {
        resources->expected_total_output_frames = -1;
    }

    return true;
}

bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    double estimated_output = ceil((double)BUFFER_SIZE_SAMPLES * (double)resample_ratio);
    resources->max_out_samples = (unsigned int)estimated_output + RESAMPLER_OUTPUT_SAFETY_MARGIN;

    double upper_limit = 20.0 * (double)BUFFER_SIZE_SAMPLES * fmax(1.0, (double)resample_ratio);
    if (resources->max_out_samples > upper_limit || resources->max_out_samples > (UINT_MAX - 128)) {
        log_fatal("Error: Calculated output buffer size per item (%u samples) is unreasonable.", resources->max_out_samples);
        return false;
    }

    size_t raw_input_bytes_per_item = BUFFER_SIZE_SAMPLES * resources->input_bytes_per_sample_pair;
    size_t complex_pre_resample_elements = BUFFER_SIZE_SAMPLES;
    size_t complex_resampled_elements = resources->max_out_samples;
    size_t complex_post_resample_elements = (config->shift_after_resample) ? resources->max_out_samples : 0;

    resources->output_bytes_per_sample_pair = get_bytes_per_sample(config->output_format);
    size_t final_output_bytes_per_item = resources->max_out_samples * resources->output_bytes_per_sample_pair;

    resources->sample_chunk_pool = malloc(NUM_BUFFERS * sizeof(SampleChunk));
    resources->raw_input_data_pool = malloc(NUM_BUFFERS * raw_input_bytes_per_item);
    resources->complex_pre_resample_data_pool = malloc(NUM_BUFFERS * complex_pre_resample_elements * sizeof(complex_float_t));
    resources->complex_resampled_data_pool = malloc(NUM_BUFFERS * complex_resampled_elements * sizeof(complex_float_t));
    if (complex_post_resample_elements > 0) {
        resources->complex_post_resample_data_pool = malloc(NUM_BUFFERS * complex_post_resample_elements * sizeof(complex_float_t));
    } else {
        resources->complex_post_resample_data_pool = NULL;
    }
    resources->final_output_data_pool = malloc(NUM_BUFFERS * final_output_bytes_per_item);

    if (!resources->sample_chunk_pool || !resources->raw_input_data_pool || !resources->complex_pre_resample_data_pool ||
        (complex_post_resample_elements > 0 && !resources->complex_post_resample_data_pool) ||
        !resources->complex_resampled_data_pool || !resources->final_output_data_pool) {
        log_fatal("Error: Failed to allocate one or more processing buffer pools.");
        return false;
    }

    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        SampleChunk* item = &resources->sample_chunk_pool[i];
        item->raw_input_data = (char*)resources->raw_input_data_pool + i * raw_input_bytes_per_item;
        item->complex_pre_resample_data = resources->complex_pre_resample_data_pool + i * complex_pre_resample_elements;
        item->complex_resampled_data = resources->complex_resampled_data_pool + i * complex_resampled_elements;
        item->final_output_data = (unsigned char*)resources->final_output_data_pool + i * final_output_bytes_per_item;

        if (complex_post_resample_elements > 0) {
            item->complex_post_resample_data = resources->complex_post_resample_data_pool + i * complex_post_resample_elements;
        } else {
            item->complex_post_resample_data = NULL;
        }
    }

    return true;
}

bool create_dsp_components(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    if (!shift_create_ncos(config, resources)) {
        return false;
    }

    if (!resources->is_passthrough) {
        resources->resampler = msresamp_crcf_create(resample_ratio, STOPBAND_ATTENUATION_DB);
        if (!resources->resampler) {
            log_fatal("Error: Failed to create liquid-dsp resampler object.");
            shift_destroy_ncos(resources);
            return false;
        }
    }
    return true;
}

bool create_threading_components(AppResources *resources) {
    resources->free_sample_chunk_queue = queue_create(NUM_BUFFERS);
    resources->raw_to_pre_process_queue = queue_create(NUM_BUFFERS);
    resources->pre_process_to_resampler_queue = queue_create(NUM_BUFFERS);
    resources->resampler_to_post_process_queue = queue_create(NUM_BUFFERS);
    resources->stdout_queue = queue_create(NUM_BUFFERS); // <<< ADDED
    if (resources->config->iq_correction.enable) {
        resources->iq_optimization_data_queue = queue_create(NUM_BUFFERS);
    } else {
        resources->iq_optimization_data_queue = NULL;
    }

    if (!resources->free_sample_chunk_queue || !resources->raw_to_pre_process_queue ||
        !resources->pre_process_to_resampler_queue || !resources->resampler_to_post_process_queue ||
        !resources->stdout_queue || // <<< ADDED
        (resources->config->iq_correction.enable && !resources->iq_optimization_data_queue)) {
        log_fatal("Failed to create one or more processing queues.");
        return false;
    }

    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        if (!queue_enqueue(resources->free_sample_chunk_queue, &resources->sample_chunk_pool[i])) {
            log_fatal("Failed to initially populate free item queue.");
            return false;
        }
    }

    if (pthread_mutex_init(&resources->progress_mutex, NULL) != 0) {
        log_fatal("Failed to initialize progress mutex: %s", strerror(errno));
        return false;
    }

    return true;
}

void destroy_threading_components(AppResources *resources) {
    if(resources->free_sample_chunk_queue) queue_destroy(resources->free_sample_chunk_queue);
    if(resources->raw_to_pre_process_queue) queue_destroy(resources->raw_to_pre_process_queue);
    if(resources->pre_process_to_resampler_queue) queue_destroy(resources->pre_process_to_resampler_queue);
    if(resources->resampler_to_post_process_queue) queue_destroy(resources->resampler_to_post_process_queue);
    if(resources->stdout_queue) queue_destroy(resources->stdout_queue); // <<< ADDED
    if(resources->iq_optimization_data_queue) queue_destroy(resources->iq_optimization_data_queue);
    pthread_mutex_destroy(&resources->progress_mutex);
}

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

    const char* base_output_labels[] = {
        "Output Type", "Sample Type", "Output Rate", "Gain", "Frequency Shift",
        "I/Q Correction", "DC Block", "Resampling", "Output Target"
    };
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
            fprintf(stderr, " %-*s : %s\n", max_label_len, summary_info.items[i].label, summary_info.items[i].value);
        }
    }

    fprintf(stderr, "--- Output Details ---\n");
    const char* output_type_str;
    switch (config->output_type) {
        case OUTPUT_TYPE_RAW: output_type_str = "RAW"; break;
        case OUTPUT_TYPE_WAV: output_type_str = "WAV"; break;
        case OUTPUT_TYPE_WAV_RF64: output_type_str = "WAV (RF64)"; break;
        default: output_type_str = "Unknown"; break;
    }
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Output Type", output_type_str);

    const char* sample_type_str = utils_get_format_description_string(config->output_format);
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Sample Type", sample_type_str);

    fprintf(stderr, " %-*s : %.0f Hz\n", max_label_len, "Output Rate", config->target_rate);
    fprintf(stderr, " %-*s : %.5f\n", max_label_len, "Gain", config->gain);

    if (config->set_center_frequency_target_hz) {
        fprintf(stderr, " %-*s : %.0f Hz\n", max_label_len, "Target Frequency", config->center_frequency_target_hz);
    }
    if (fabs(resources->actual_nco_shift_hz) > 1e-9) {
        char shift_buf[64];
        snprintf(shift_buf, sizeof(shift_buf), "%+.2f Hz%s", resources->actual_nco_shift_hz, config->shift_after_resample ? " (Post-Resample)" : "");
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Frequency Shift", shift_buf);
    }

    fprintf(stderr, " %-*s : %s\n", max_label_len, "I/Q Correction", config->iq_correction.enable ? "Enabled" : "Disabled");
    fprintf(stderr, " %-*s : %s\n", max_label_len, "DC Block", config->dc_block.enable ? "Enabled" : "Disabled");
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Resampling", resources->is_passthrough ? "Disabled (Passthrough Mode)" : "Enabled");

    const char* output_path_for_messages;
#ifdef _WIN32
    output_path_for_messages = config->effective_output_filename_utf8;
#else
    output_path_for_messages = config->effective_output_filename;
#endif
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Output Target", config->output_to_stdout ? "<stdout>" : output_path_for_messages);
}

bool check_nyquist_warning(const AppConfig *config, const AppResources *resources) {
    return shift_check_nyquist_warning(config, resources);
}

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

bool initialize_application(AppConfig *config, AppResources *resources) {
    bool success = false;
    resources->config = config;
    InputSourceContext ctx = { .config = config, .resources = resources };
    float resample_ratio = 0.0f;

    if (!resolve_file_paths(config)) goto cleanup;
    if (!resources->selected_input_ops->initialize(&ctx)) goto cleanup;
    if (!calculate_and_validate_resample_ratio(config, resources, &resample_ratio)) goto cleanup;
    if (!allocate_processing_buffers(config, resources, resample_ratio)) goto cleanup;
    if (!create_dsp_components(config, resources, resample_ratio)) goto cleanup;
    if (!create_threading_components(resources)) goto cleanup;

    // Conditionally create the large I/O buffer only if writing to a file.
    if (!config->output_to_stdout) {
        resources->file_write_buffer = file_write_buffer_create(IO_RING_BUFFER_CAPACITY);
        if (!resources->file_write_buffer) {
            log_fatal("Failed to create I/O buffer.");
            goto cleanup;
        }
    } else {
        resources->file_write_buffer = NULL; // Ensure it's NULL for the stdout path
    }

    if (!config->output_to_stdout) {
        print_configuration_summary(config, resources);
        if (!check_nyquist_warning(config, resources)) {
            goto cleanup;
        }
    }

    if (!prepare_output_stream(config, resources)) goto cleanup;

    if (config->dc_block.enable) {
        if (!dc_block_init(config, resources)) goto cleanup;
    }

    if (config->iq_correction.enable) {
        if (!iq_correct_init(config, resources)) goto cleanup;
    }

    success = true;

cleanup:
    if (!success) {
        log_error("Application initialization failed.");
    } else if (!config->output_to_stdout) {
        bool source_has_known_length = resources->selected_input_ops->has_known_length();
        if (!source_has_known_length) {
            log_info("Starting SDR capture...");
        } else {
            log_info("Starting file processing...");
        }
    }
    return success;
}

void cleanup_application(AppConfig *config, AppResources *resources) {
    if (!resources) return;
    InputSourceContext ctx = { .config = config, .resources = resources };

    if (resources->selected_input_ops && resources->selected_input_ops->cleanup) {
        resources->selected_input_ops->cleanup(&ctx);
    }

    if (resources->writer_ctx.ops.close) {
        resources->writer_ctx.ops.close(&resources->writer_ctx);
    }
    if (resources->writer_ctx.ops.get_total_bytes_written) {
        resources->final_output_size_bytes = resources->writer_ctx.ops.get_total_bytes_written(&resources->writer_ctx);
    }

    if (config->dc_block.enable) {
        dc_block_cleanup(resources);
    }
    if (config->iq_correction.enable) {
        iq_correct_cleanup(resources);
    }

    if (resources->file_write_buffer) {
        file_write_buffer_destroy(resources->file_write_buffer);
        resources->file_write_buffer = NULL;
    }

    destroy_threading_components(resources);
    shift_destroy_ncos(resources);

    if (resources->resampler) {
        msresamp_crcf_destroy(resources->resampler);
        resources->resampler = NULL;
    }

    free(resources->sample_chunk_pool);
    free(resources->raw_input_data_pool);
    free(resources->complex_pre_resample_data_pool);
    free(resources->complex_post_resample_data_pool);
    free(resources->complex_resampled_data_pool);
    free(resources->final_output_data_pool);

#ifdef _WIN32
    free_absolute_path_windows(&config->effective_input_filename_w, &config->effective_input_filename_utf8);
    free_absolute_path_windows(&config->effective_output_filename_w, &config->effective_output_filename_utf8);
#endif
}
