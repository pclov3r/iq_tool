#include "setup.h"
#include "types.h"
#include "constants.h"
#include "platform.h"
#include "utils.h"
#include "frequency_shift.h"
#include "log.h"
#include "input_source.h"
#include "input_manager.h"
#include "file_writer.h"
#include "sample_convert.h"
#include "iq_correct.h"
#include "dc_block.h"
#include "filter.h"
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
#include <strings.h>
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

bool validate_and_configure_filter_stage(AppConfig *config, AppResources *resources) {
    config->apply_user_filter_post_resample = false;

    if (config->num_filter_requests == 0 || config->no_resample || config->raw_passthrough) {
        return true;
    }

    double input_rate = (double)resources->source_info.samplerate;
    double output_rate = config->target_rate;

    if (output_rate < input_rate) {
        float max_filter_freq_hz = 0.0f;

        for (int i = 0; i < config->num_filter_requests; i++) {
            const FilterRequest* req = &config->filter_requests[i];
            float current_max = 0.0f;
            switch (req->type) {
                case FILTER_TYPE_LOWPASS:
                case FILTER_TYPE_HIGHPASS:
                    current_max = fabsf(req->freq1_hz);
                    break;
                case FILTER_TYPE_PASSBAND:
                case FILTER_TYPE_STOPBAND:
                    current_max = fabsf(req->freq1_hz) + (req->freq2_hz / 2.0f);
                    break;
                default:
                    break;
            }
            if (current_max > max_filter_freq_hz) {
                max_filter_freq_hz = current_max;
            }
        }

        double output_nyquist = output_rate / 2.0;

        if (max_filter_freq_hz > output_nyquist) {
            log_fatal("Filter configuration is incompatible with the output sample rate.");
            log_error("The specified filter chain extends to %.0f Hz, but the output rate of %.0f Hz can only support frequencies up to %.0f Hz.",
                      max_filter_freq_hz, output_rate, output_nyquist);
            return false;
        } else {
            log_debug("Filter will be applied efficiently after resampling to avoid excessive CPU usage.");
            config->apply_user_filter_post_resample = true;
        }
    }
    return true;
}


bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    // --- FIX START ---
    // Correctly calculate the maximum buffer size needed at any point in the pipeline.

    // 1. Determine the maximum size of a chunk *before* the resampler.
    // This is usually the input chunk size, but can be larger if a pre-resample FFT filter is used.
    size_t max_pre_resample_chunk_size = PIPELINE_INPUT_CHUNK_SIZE_SAMPLES;
    bool is_pre_fft_filter = (resources->user_fir_filter_object && !config->apply_user_filter_post_resample &&
                             (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || 
                              resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC));

    if (is_pre_fft_filter) {
        if (resources->user_filter_block_size > max_pre_resample_chunk_size) {
            max_pre_resample_chunk_size = resources->user_filter_block_size;
        }
    }

    // 2. Calculate the maximum possible output size from the resampler, based on the
    //    largest possible input chunk it might receive.
    size_t resampler_output_capacity = (size_t)ceil((double)max_pre_resample_chunk_size * fmax(1.0, (double)resample_ratio)) + RESAMPLER_OUTPUT_SAFETY_MARGIN;

    // 3. The final capacity for all complex buffers must be the largest of the pre-resample size
    //    and the post-resample size.
    size_t required_capacity = (max_pre_resample_chunk_size > resampler_output_capacity) ? max_pre_resample_chunk_size : resampler_output_capacity;

    // 4. Also consider the post-resample FFT filter block size, if any.
    bool is_post_fft_filter = (resources->user_fir_filter_object && config->apply_user_filter_post_resample &&
                              (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || 
                               resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC));
    
    if (is_post_fft_filter) {
        if (resources->user_filter_block_size > required_capacity) {
            required_capacity = resources->user_filter_block_size;
        }
    }

    // Sanity check against a hard limit to prevent extreme memory allocation.
    if (required_capacity > MAX_ALLOWED_FFT_BLOCK_SIZE) {
        log_fatal("Error: Pipeline requires a buffer size (%zu) that exceeds the maximum allowed size (%d).",
                  required_capacity, MAX_ALLOWED_FFT_BLOCK_SIZE);
        return false;
    }
    // --- FIX END ---

    resources->max_out_samples = required_capacity;
    log_debug("Calculated required processing buffer capacity: %u samples.", resources->max_out_samples);

    size_t raw_input_bytes_per_item = PIPELINE_INPUT_CHUNK_SIZE_SAMPLES * resources->input_bytes_per_sample_pair;
    size_t complex_elements_per_item = resources->max_out_samples;
    resources->output_bytes_per_sample_pair = get_bytes_per_sample(config->output_format);
    size_t final_output_bytes_per_item = complex_elements_per_item * resources->output_bytes_per_sample_pair;

    resources->sample_chunk_pool = malloc(NUM_BUFFERS * sizeof(SampleChunk));
    resources->raw_input_data_pool = malloc(NUM_BUFFERS * raw_input_bytes_per_item);
    resources->complex_pre_resample_data_pool = malloc(NUM_BUFFERS * complex_elements_per_item * sizeof(complex_float_t));
    resources->complex_resampled_data_pool = malloc(NUM_BUFFERS * complex_elements_per_item * sizeof(complex_float_t));
    
    if (config->shift_after_resample || config->apply_user_filter_post_resample) {
        resources->complex_post_resample_data_pool = malloc(NUM_BUFFERS * complex_elements_per_item * sizeof(complex_float_t));
    } else {
        resources->complex_post_resample_data_pool = NULL;
    }
    
    resources->complex_scratch_data_pool = malloc(NUM_BUFFERS * complex_elements_per_item * sizeof(complex_float_t));
    
    resources->final_output_data_pool = malloc(NUM_BUFFERS * final_output_bytes_per_item);

    if (!resources->sample_chunk_pool || !resources->raw_input_data_pool || !resources->complex_pre_resample_data_pool ||
        (resources->complex_post_resample_data_pool == NULL && (config->shift_after_resample || config->apply_user_filter_post_resample)) ||
        !resources->complex_resampled_data_pool || !resources->complex_scratch_data_pool || !resources->final_output_data_pool) {
        log_fatal("Error: Failed to allocate one or more processing buffer pools.");
        return false;
    }

    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        SampleChunk* item = &resources->sample_chunk_pool[i];
        
        item->raw_input_data = (char*)resources->raw_input_data_pool + i * raw_input_bytes_per_item;
        item->complex_pre_resample_data = resources->complex_pre_resample_data_pool + i * complex_elements_per_item;
        item->complex_resampled_data = resources->complex_resampled_data_pool + i * complex_elements_per_item;
        item->complex_scratch_data = resources->complex_scratch_data_pool + i * complex_elements_per_item;
        item->final_output_data = (unsigned char*)resources->final_output_data_pool + i * final_output_bytes_per_item;
        
        if (resources->complex_post_resample_data_pool) {
            item->complex_post_resample_data = resources->complex_post_resample_data_pool + i * complex_elements_per_item;
        } else {
            item->complex_post_resample_data = NULL;
        }
        
        item->raw_input_capacity_bytes = raw_input_bytes_per_item;
        item->complex_buffer_capacity_samples = complex_elements_per_item;
        item->final_output_capacity_bytes = final_output_bytes_per_item;

        item->input_bytes_per_sample_pair = resources->input_bytes_per_sample_pair;
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
    
    if (!filter_create(config, resources)) {
        shift_destroy_ncos(resources);
        if (resources->resampler) msresamp_crcf_destroy(resources->resampler);
        return false;
    }

    return true;
}

bool create_threading_components(AppResources *resources) {
    resources->free_sample_chunk_queue = queue_create(NUM_BUFFERS);
    resources->raw_to_pre_process_queue = queue_create(NUM_BUFFERS);
    resources->pre_process_to_resampler_queue = queue_create(NUM_BUFFERS);
    resources->resampler_to_post_process_queue = queue_create(NUM_BUFFERS);
    resources->stdout_queue = queue_create(NUM_BUFFERS);
    if (resources->config->iq_correction.enable) {
        resources->iq_optimization_data_queue = queue_create(NUM_BUFFERS);
    } else {
        resources->iq_optimization_data_queue = NULL;
    }

    if (!resources->free_sample_chunk_queue || !resources->raw_to_pre_process_queue ||
        !resources->pre_process_to_resampler_queue || !resources->resampler_to_post_process_queue ||
        !resources->stdout_queue ||
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
    if(resources->stdout_queue) queue_destroy(resources->stdout_queue);
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
        "I/Q Correction", "DC Block", "Resampling", "Output Target", "FIR Filter"
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

    fprintf(stderr, "\n--- Input Details ---\n");
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
    
    if (config->num_filter_requests == 0) {
        fprintf(stderr, " %-*s : %s\n", max_label_len, "FIR Filter", "Disabled");
    } else {
        char filter_buf[256] = {0};
        const char* stage = config->apply_user_filter_post_resample ? " (Post-Resample)" : "";
        strncat(filter_buf, "Enabled: ", sizeof(filter_buf) - strlen(filter_buf) - 1);
        for (int i = 0; i < config->num_filter_requests; i++) {
            char current_filter_desc[128];
            const FilterRequest* req = &config->filter_requests[i];
            switch (req->type) {
                case FILTER_TYPE_LOWPASS: snprintf(current_filter_desc, sizeof(current_filter_desc), "LPF(%.0f Hz)", req->freq1_hz); break;
                case FILTER_TYPE_HIGHPASS: snprintf(current_filter_desc, sizeof(current_filter_desc), "HPF(%.0f Hz)", req->freq1_hz); break;
                case FILTER_TYPE_PASSBAND: snprintf(current_filter_desc, sizeof(current_filter_desc), "BPF(%.0f Hz, BW %.0f Hz)", req->freq1_hz, req->freq2_hz); break;
                case FILTER_TYPE_STOPBAND: snprintf(current_filter_desc, sizeof(current_filter_desc), "BSF(%.0f Hz, BW %.0f Hz)", req->freq1_hz, req->freq2_hz); break;
                default: break;
            }
            if (i > 0) strncat(filter_buf, " + ", sizeof(filter_buf) - strlen(filter_buf) - 1);
            strncat(filter_buf, current_filter_desc, sizeof(filter_buf) - strlen(filter_buf) - 1);
        }
        strncat(filter_buf, stage, sizeof(filter_buf) - strlen(filter_buf) - 1);
        fprintf(stderr, " %-*s : %s\n", max_label_len, "FIR Filter", filter_buf);
    }

    fprintf(stderr, " %-*s : %s\n", max_label_len, "Resampling", resources->is_passthrough ? "Disabled (Passthrough Mode)" : "Enabled");

    const char* output_path_for_messages;
#ifdef _WIN32
    output_path_for_messages = config->effective_output_filename_utf8;
#else
    output_path_for_messages = config->effective_output_filename;
#endif
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Output Target", config->output_to_stdout ? "<stdout>" : output_path_for_messages);
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

    bool is_sdr = is_sdr_input(config->input_type_str);
    if (is_sdr) {
        if (config->output_to_stdout) {
            resources->pipeline_mode = PIPELINE_MODE_REALTIME_SDR;
            log_debug("SDR to stdout: Real-time, low-latency mode enabled.");
        } else {
            resources->pipeline_mode = PIPELINE_MODE_BUFFERED_SDR;
            log_debug("SDR to file: Buffered, max-quality mode enabled.");
        }
    } else {
        resources->pipeline_mode = PIPELINE_MODE_FILE_PROCESSING;
        log_debug("File processing: Self-paced, max-quality mode enabled.");
    }

    if (!resolve_file_paths(config)) goto cleanup;
    if (!resources->selected_input_ops->initialize(&ctx)) goto cleanup;
    if (!calculate_and_validate_resample_ratio(config, resources, &resample_ratio)) goto cleanup;
    if (!validate_and_configure_filter_stage(config, resources)) goto cleanup;
    
    if (!create_dsp_components(config, resources, resample_ratio)) goto cleanup;
    if (!allocate_processing_buffers(config, resources, resample_ratio)) goto cleanup;
    
    if (!create_threading_components(resources)) goto cleanup;

    if (resources->pipeline_mode == PIPELINE_MODE_BUFFERED_SDR) {
        resources->sdr_input_buffer = file_write_buffer_create(SDR_INPUT_BUFFER_CAPACITY);
        if (!resources->sdr_input_buffer) {
            log_fatal("Failed to create SDR input buffer for buffered mode.");
            goto cleanup;
        }
    }

    if (!config->output_to_stdout) {
        resources->file_write_buffer = file_write_buffer_create(IO_RING_BUFFER_CAPACITY);
        if (!resources->file_write_buffer) {
            log_fatal("Failed to create I/O output buffer.");
            goto cleanup;
        }
    } else {
        resources->file_write_buffer = NULL;
    }

    if (!config->output_to_stdout) {
        print_configuration_summary(config, resources);
        
        if (fabs(resources->actual_nco_shift_hz) > 1e-9) {
            double rate_for_shift_check = config->shift_after_resample ? config->target_rate : (double)resources->source_info.samplerate;
            if (!utils_check_nyquist_warning(fabs(resources->actual_nco_shift_hz), rate_for_shift_check, "Frequency Shift")) {
                goto cleanup;
            }
        }

        if (config->num_filter_requests > 0) {
            double rate_for_filter_check = config->apply_user_filter_post_resample ? config->target_rate : (double)resources->source_info.samplerate;
            
            for (int i = 0; i < config->num_filter_requests; i++) {
                const FilterRequest* req = &config->filter_requests[i];
                double freq_to_check = 0.0;
                const char* context = NULL;

                switch (req->type) {
                    case FILTER_TYPE_LOWPASS:
                    case FILTER_TYPE_HIGHPASS:
                        freq_to_check = req->freq1_hz;
                        context = "Filter Cutoff";
                        break;
                    case FILTER_TYPE_PASSBAND:
                    case FILTER_TYPE_STOPBAND:
                        freq_to_check = fabsf(req->freq1_hz) + (req->freq2_hz / 2.0f);
                        context = "Filter Edge";
                        break;
                    default:
                        break;
                }

                if (context && !utils_check_nyquist_warning(freq_to_check, rate_for_filter_check, context)) {
                    goto cleanup;
                }
            }
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
    if (success && !config->output_to_stdout) {
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

    if (config->dc_block.enable) {
        dc_block_cleanup(resources);
    }
    if (config->iq_correction.enable) {
        iq_correct_cleanup(resources);
    }
    filter_destroy(resources);
    shift_destroy_ncos(resources);
    if (resources->resampler) {
        msresamp_crcf_destroy(resources->resampler);
        resources->resampler = NULL;
    }

    if (resources->selected_input_ops && resources->selected_input_ops->cleanup) {
        resources->selected_input_ops->cleanup(&ctx);
    }

    if (resources->writer_ctx.ops.close) {
        resources->writer_ctx.ops.close(&resources->writer_ctx);
    }
    if (resources->writer_ctx.ops.get_total_bytes_written) {
        resources->final_output_size_bytes = resources->writer_ctx.ops.get_total_bytes_written(&resources->writer_ctx);
    }

    if (resources->sdr_input_buffer) {
        file_write_buffer_destroy(resources->sdr_input_buffer);
        resources->sdr_input_buffer = NULL;
    }

    if (resources->file_write_buffer) {
        file_write_buffer_destroy(resources->file_write_buffer);
        resources->file_write_buffer = NULL;
    }

    destroy_threading_components(resources);

    free(resources->sample_chunk_pool);
    free(resources->raw_input_data_pool);
    free(resources->complex_pre_resample_data_pool);
    free(resources->complex_post_resample_data_pool);
    free(resources->complex_scratch_data_pool);
    free(resources->complex_resampled_data_pool);
    free(resources->final_output_data_pool);

#ifdef _WIN32
    free_absolute_path_windows(&config->effective_input_filename_w, &config->effective_input_filename_utf8);
    free_absolute_path_windows(&config->effective_output_filename_w, &config->effective_output_filename_utf8);
#endif
}
