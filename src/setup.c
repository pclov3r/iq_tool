#include "setup.h"
#include "constants.h"
#include "platform.h"
#include "utils.h"
#include "log.h"
#include "module.h"
#include "module_manager.h"
#include "output_writer.h"
#include "pipeline.h"
#include "app_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#include <strings.h>
#include <limits.h>
#include <stdlib.h>
#endif


bool resolve_file_paths(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

#ifdef _WIN32
    if (config->input_filename_arg) {
        if (!get_absolute_path_windows(config->input_filename_arg,
                                       config->effective_input_filename_w, MAX_PATH_BUFFER,
                                       config->effective_input_filename_utf8, MAX_PATH_BUFFER)) {
            return false;
        }
    }
    if (config->output_filename_arg) {
        if (!get_absolute_path_windows(config->output_filename_arg,
                                       config->effective_output_filename_w, MAX_PATH_BUFFER,
                                       config->effective_output_filename_utf8, MAX_PATH_BUFFER)) {
            return false;
        }
    }
#else
    if (config->input_filename_arg) {
        char resolved_input_path[PATH_MAX];
        if (realpath(config->input_filename_arg, resolved_input_path) == NULL) {
            log_fatal("Input file not found or path is invalid: %s (%s)", config->input_filename_arg, strerror(errno));
            return false;
        }
        config->effective_input_filename = mem_arena_alloc(&resources->setup_arena, strlen(resolved_input_path) + 1, false);
        if (!config->effective_input_filename) return false;
        strcpy(config->effective_input_filename, resolved_input_path);
    }

    if (config->output_filename_arg) {
        char* path_copy_for_dirname = mem_arena_alloc(&resources->setup_arena, strlen(config->output_filename_arg) + 1, false);
        char* path_copy_for_basename = mem_arena_alloc(&resources->setup_arena, strlen(config->output_filename_arg) + 1, false);
        if (!path_copy_for_dirname || !path_copy_for_basename) return false;

        strcpy(path_copy_for_dirname, config->output_filename_arg);
        strcpy(path_copy_for_basename, config->output_filename_arg);

        char* dir = dirname(path_copy_for_dirname);
        char* base = basename(path_copy_for_basename);

        char resolved_dir_path[PATH_MAX];
        if (realpath(dir, resolved_dir_path) == NULL) {
            log_fatal("Output directory does not exist or path is invalid: %s (%s)", dir, strerror(errno));
            return false;
        }

        size_t final_len = strlen(resolved_dir_path) + 1 + strlen(base) + 1;
        config->effective_output_filename = mem_arena_alloc(&resources->setup_arena, final_len, false);
        if (!config->effective_output_filename) return false;

        snprintf(config->effective_output_filename, final_len, "%s/%s", resolved_dir_path, base);
    }
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

void print_configuration_summary(const AppConfig *config, const AppResources *resources) {
    if (!config || !resources || !resources->selected_input_module_api) return;

    InputSummaryInfo summary_info;
    memset(&summary_info, 0, sizeof(InputSummaryInfo));
    const ModuleContext ctx = { .config = config, .resources = (AppResources*)resources };
    resources->selected_input_module_api->get_summary_info(&ctx, &summary_info);

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
        "Container Type", "Sample Type", "Output Rate", "Gain Multiplier", "Frequency Shift",
        "Resampling", "Output Target", "FIR Filter", "FFT Filter"
    };
    for (size_t i = 0; i < sizeof(base_output_labels) / sizeof(base_output_labels[0]); i++) {
        int len = (int)strlen(base_output_labels[i]);
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
    
    fprintf(stderr, " %-*s : %s\n", max_label_len, "I/Q Correction", config->iq_correction.enable ? "Enabled" : "Disabled");
    fprintf(stderr, " %-*s : %s\n", max_label_len, "DC Block", config->dc_block.enable ? "Enabled" : "Disabled");


    fprintf(stderr, "--- Output Details ---\n");
    const char* output_type_str;
    switch (config->output_type) {
        case OUTPUT_TYPE_RAW: output_type_str = "RAW"; break;
        case OUTPUT_TYPE_WAV: output_type_str = "WAV"; break;
        case OUTPUT_TYPE_WAV_RF64: output_type_str = "WAV (RF64)"; break;
        default: output_type_str = "Unknown"; break;
    }
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Container Type", output_type_str);

    const char* sample_type_str = utils_get_format_description_string(config->output_format);
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Sample Type", sample_type_str);

    fprintf(stderr, " %-*s : %.0f Hz\n", max_label_len, "Output Rate", config->target_rate);
    fprintf(stderr, " %-*s : %.5f\n", max_label_len, "Gain Multiplier", config->gain);

    if (fabs(resources->nco_shift_hz) > 1e-9) {
        char shift_buf[64];
        snprintf(shift_buf, sizeof(shift_buf), "%+.2f Hz%s", resources->nco_shift_hz, config->shift_after_resample ? " (Post-Resample)" : "");
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Frequency Shift", shift_buf);
    }

    if (config->num_filter_requests == 0) {
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Filter", "Disabled");
    } else {
        const char* filter_label;
        switch (resources->user_filter_type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
            case FILTER_IMPL_FIR_ASYMMETRIC:
                filter_label = "FIR Filter";
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
            case FILTER_IMPL_FFT_ASYMMETRIC:
                filter_label = "FFT Filter";
                break;
            default:
                filter_label = "Filter";
                break;
        }
        
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
        fprintf(stderr, " %-*s : %s\n", max_label_len, filter_label, filter_buf);
    }

    fprintf(stderr, " %-*s : %s\n", max_label_len, "Resampling", resources->is_passthrough ? "Disabled (Passthrough Mode)" : "Enabled");

    const char* output_path_for_messages;
#ifdef _WIN32
    output_path_for_messages = config->effective_output_filename_utf8;
#else
    output_path_for_messages = config->effective_output_filename;
#endif
    fprintf(stderr, " %-*s : %s\n", max_label_len, config->output_to_stdout ? "Output Target" : "Output File", config->output_to_stdout ? "<stdout>" : output_path_for_messages);
}

bool prepare_output_stream(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    if (!output_writer_init(&resources->writer_ctx, config)) {
        return false;
    }

    if (!resources->writer_ctx.api.open(&resources->writer_ctx, config, resources, &resources->setup_arena)) {
        return false;
    }
    return true;
}

bool initialize_application(AppConfig *config, AppResources *resources) {
    resources->config = config;
    ModuleContext ctx = { .config = config, .resources = resources };
    
    // --- PRE-FLIGHT CHECKS ---
    // STEP 1: Resolve file paths (if any)
    if (!resolve_file_paths(config, resources)) {
        return false;
    }

    // STEP 2: Initialize the external source (SDR or file) to get its properties
    if (!resources->selected_input_module_api->initialize(&ctx)) {
        return false;
    }

    // STEP 3: Perform prerequisite calculations based on source properties
    if (!calculate_and_validate_resample_ratio(config, resources, &resources->resample_ratio)) {
        cleanup_application(config, resources);
        return false;
    }

    // --- FINAL PREPARATION ---
    // STEP 4: Perform optional pre-stream calibration for file sources
    if (resources->selected_input_module_api->pre_stream_iq_correction) {
        if (!resources->selected_input_module_api->pre_stream_iq_correction(&ctx)) {
            cleanup_application(config, resources);
            return false;
        }
    }

    // STEP 5: Print summary (UI task)
    if (!config->output_to_stdout) {
        print_configuration_summary(config, resources);
    }

    // STEP 6: Prepare the final output destination
    if (!prepare_output_stream(config, resources)) {
        cleanup_application(config, resources);
        return false;
    }

    if (!config->output_to_stdout) {
        bool source_has_known_length = resources->selected_input_module_api->has_known_length();
        if (!source_has_known_length) {
            log_info("Starting SDR capture...");
        } else {
            log_info("Starting file processing...");
        }
    }

    return true;
}

void cleanup_application(AppConfig *config, AppResources *resources) {
    if (!resources) return;
    ModuleContext ctx = { .config = config, .resources = resources };

    // Clean up the high-level resources managed by setup.
    // The pipeline_run function is now responsible for its own internal cleanup.
    if (resources->writer_ctx.api.close) {
        resources->writer_ctx.api.close(&resources->writer_ctx);
        if (resources->writer_ctx.api.get_total_bytes_written) {
            resources->final_output_size_bytes = resources->writer_ctx.api.get_total_bytes_written(&resources->writer_ctx);
        }
    }

    if (resources->pipeline_chunk_data_pool) {
        free(resources->pipeline_chunk_data_pool);
        resources->pipeline_chunk_data_pool = NULL;
    }
    
    if (resources->selected_input_module_api && resources->selected_input_module_api->cleanup) {
        resources->selected_input_module_api->cleanup(&ctx);
    }
}
