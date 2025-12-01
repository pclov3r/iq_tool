#include "setup.h"
#include "sample_convert.h"
#include "constants.h"
#include "platform.h"
#include "utils.h"
#include "log.h"
#include "module.h"
#include "module_manager.h"
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
#include <malloc.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#include <strings.h>
#include <limits.h>
#include <stdlib.h>
#endif

// Helper macro to align a size up to the next power of 2 boundary (e.g. 32 bytes)
#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))


bool resolve_file_paths(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

#ifdef _WIN32
    if (config->input.path_arg) {
        if (!get_absolute_path_windows(config->input.path_arg,
                                       config->input.effective_path_w, MAX_PATH_BUFFER,
                                       config->input.effective_path_utf8, MAX_PATH_BUFFER)) {
            return false;
        }
    }
    if (config->output.path_arg) {
        if (!get_absolute_path_windows(config->output.path_arg,
                                       config->output.effective_path_w, MAX_PATH_BUFFER,
                                       config->output.effective_path_utf8, MAX_PATH_BUFFER)) {
            return false;
        }
    }
#else
    if (config->input.path_arg) {
        char resolved_input_path[PATH_MAX];
        if (realpath(config->input.path_arg, resolved_input_path) == NULL) {
            log_fatal("Input file not found or path is invalid: %s (%s)", config->input.path_arg, strerror(errno));
            return false;
        }
        config->input.effective_path = mem_arena_alloc(&resources->setup_arena, strlen(resolved_input_path) + 1, false);
        if (!config->input.effective_path) return false;
        strcpy(config->input.effective_path, resolved_input_path);
    }

    if (config->output.path_arg) {
        char* path_copy_for_dirname = mem_arena_alloc(&resources->setup_arena, strlen(config->output.path_arg) + 1, false);
        char* path_copy_for_basename = mem_arena_alloc(&resources->setup_arena, strlen(config->output.path_arg) + 1, false);
        if (!path_copy_for_dirname || !path_copy_for_basename) return false;

        strcpy(path_copy_for_dirname, config->output.path_arg);
        strcpy(path_copy_for_basename, config->output.path_arg);

        char* dir = dirname(path_copy_for_dirname);
        char* base = basename(path_copy_for_basename);

        char resolved_dir_path[PATH_MAX];
        if (realpath(dir, resolved_dir_path) == NULL) {
            log_fatal("Output directory does not exist or path is invalid: %s (%s)", dir, strerror(errno));
            return false;
        }

        size_t final_len = strlen(resolved_dir_path) + 1 + strlen(base) + 1;
        config->output.effective_path = mem_arena_alloc(&resources->setup_arena, final_len, false);
        if (!config->output.effective_path) return false;

        snprintf(config->output.effective_path, final_len, "%s/%s", resolved_dir_path, base);
    }
#endif
    return true;
}

bool calculate_and_validate_resample_ratio(AppConfig *config, AppResources *resources, float *out_ratio) {
    if (!config || !resources || !out_ratio) return false;

    if (config->dsp.no_resample || config->dsp.raw_passthrough) {
        if (config->dsp.raw_passthrough) {
            log_info("Raw Passthrough mode enabled: Bypassing all DSP blocks.");
        } else {
            log_info("Native rate processing enabled: output rate will match input rate.");
        }
        config->output_rate.target_rate = (double)resources->source_info.samplerate;
        resources->is_passthrough = true;
    } else {
        resources->is_passthrough = false;
    }

    double input_rate_d = (double)resources->source_info.samplerate;
    float r = (float)(config->output_rate.target_rate / input_rate_d);

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

bool validate_and_configure_filter_stage(struct AppConfig *config, struct AppResources *resources) {
    config->dsp.filter.apply_post_resample = false;

    if (config->dsp.filter.count == 0 || config->dsp.no_resample || config->dsp.raw_passthrough) {
        return true;
    }

    double input_rate = (double)resources->source_info.samplerate;
    double output_rate = config->output_rate.target_rate;

    // Optimization: If downsampling, filtering AFTER resampling might be more efficient
    // IF the filter frequency is within the new Nyquist limit.
    if (output_rate < input_rate) {
        float max_filter_freq_hz = 0.0f;

        for (int i = 0; i < config->dsp.filter.count; i++) {
            const FilterRequest* req = &config->dsp.filter.requests[i];
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
            // It's safe and more efficient to filter after resampling.
            log_debug("Filter will be applied efficiently after resampling to avoid excessive CPU usage.");
            config->dsp.filter.apply_post_resample = true;
        }
    }
    return true;
}

// --- ELASTIC BUFFER ALLOCATION LOGIC ---
bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    // 1. Determine the "Fat Pipe" (highest data rate side)
    //    Upsampling (Ratio > 1.0): Output is the Fat Pipe.
    //    Downsampling (Ratio <= 1.0): Input is the Fat Pipe.
    bool upsampling = (resample_ratio > 1.0f);

    size_t target_block_samples = PIPELINE_TARGET_BLOCK_SAMPLES; // 12,288 samples (~192KB)

    // 2. Adjust target for FFT requirements if necessary
    if (resources->user_filter_object) {
        // If the user has an FFT filter, the block size MUST be at least the FFT block size.
        // We prioritize functional correctness over L2 cache optimization here.
        if (resources->user_filter_block_size > target_block_samples) {
            log_debug("Adjusting pipeline block size to %u to accommodate FFT filter requirements.", resources->user_filter_block_size);
            target_block_samples = resources->user_filter_block_size;
        }
    }

    size_t calculated_input_samples = 0;
    size_t sample_allocation_count = 0;

    if (upsampling) {
        // --- CASE A: UPSAMPLING ---
        // The Output is pinned to the Target.
        // Calculate Input required: Input = Target / Ratio.
        size_t raw_input_calc = (size_t)(target_block_samples / resample_ratio);

        // Sanity Floor: Prevent tiny read requests that cause excessive locking overhead.
        if (raw_input_calc < PIPELINE_MIN_READ_SAMPLES) {
            calculated_input_samples = PIPELINE_MIN_READ_SAMPLES;
        } else {
            calculated_input_samples = raw_input_calc;
        }

        // We must allocate enough memory to hold the *Result* of this input chunk.
        // Allocation = (Input * Ratio) + Safety Margin.
        sample_allocation_count = (size_t)ceil((double)calculated_input_samples * resample_ratio) + RESAMPLER_OUTPUT_SAFETY_MARGIN;
    }
    else {
        // --- CASE B: DOWNSAMPLING / PASSTHROUGH ---
        // The Input is pinned to the Target.
        calculated_input_samples = target_block_samples;

        // Allocation = Input Size + Safety Margin.
        // (Since output shrinks, the input buffer determines the required memory pool size).
        sample_allocation_count = target_block_samples + RESAMPLER_OUTPUT_SAFETY_MARGIN;
    }

    // Store the results for runtime usage
    resources->pipeline_read_chunk_size = calculated_input_samples;
    resources->pipeline_alloc_size_samples = sample_allocation_count;

    // 3. FFT Limit Safety Check
    if (resources->pipeline_alloc_size_samples > MAX_ALLOWED_FFT_BLOCK_SIZE) {
        log_fatal("Calculated pipeline buffer size (%zu) exceeds maximum allowed FFT size (%d).", 
                  resources->pipeline_alloc_size_samples, MAX_ALLOWED_FFT_BLOCK_SIZE);
        return false;
    }

    // Update legacy field used by some filters
    resources->max_out_samples = (unsigned int)resources->pipeline_alloc_size_samples;

    // -------------------------------------------------------------------------
    // 4. Calculate Dynamic Pipeline Depth ("Trays")
    // -------------------------------------------------------------------------
    double input_rate = (double)resources->source_info.samplerate;

    // FAIL FAST: If the input rate is unknown or invalid, we cannot safely configure the pipeline.
    if (input_rate <= 0.0) {
        log_fatal("Internal Error: Input sample rate is invalid (%.0f Hz). Cannot calculate buffer depth.", input_rate);
        log_fatal("Please check the input source configuration.");
        return false;
    }

    // How much time does one chunk represent?
    double seconds_per_chunk = (double)resources->pipeline_read_chunk_size / input_rate;

    // How many chunks do we need to hit the target duration?
    size_t calculated_chunks = (size_t)(PIPELINE_TARGET_BUFFER_DURATION_SEC / seconds_per_chunk);

    // Apply Sanity Clamps
    if (calculated_chunks < PIPELINE_MIN_CHUNKS) calculated_chunks = PIPELINE_MIN_CHUNKS;
    if (calculated_chunks > PIPELINE_MAX_CHUNKS) calculated_chunks = PIPELINE_MAX_CHUNKS;

    resources->pipeline_num_chunks = calculated_chunks;

    log_info("Pipeline Sizing: Read=%zu samples, Alloc=%zu samples, Depth=%zu chunks (%.2f sec buffer at %.0f Hz)", 
              resources->pipeline_read_chunk_size,
              resources->pipeline_alloc_size_samples,
              resources->pipeline_num_chunks,
              resources->pipeline_num_chunks * seconds_per_chunk,
              input_rate);

    // --- MEMORY ALLOCATION WITH ALIGNMENT ---

    // Calculate raw byte sizes
    size_t raw_input_bytes = resources->pipeline_alloc_size_samples * resources->input_bytes_per_sample_pair;
    size_t complex_bytes = resources->pipeline_alloc_size_samples * sizeof(complex_float_t);
    resources->output_bytes_per_sample_pair = get_bytes_per_sample(config->output.format);
    size_t final_output_bytes = resources->pipeline_alloc_size_samples * resources->output_bytes_per_sample_pair;

    // Calculate Strides (Aligned to 32 bytes)
    size_t raw_stride = ALIGN_UP(raw_input_bytes, MEM_ARENA_ALIGNMENT);
    size_t complex_stride = ALIGN_UP(complex_bytes, MEM_ARENA_ALIGNMENT);
    size_t final_stride = ALIGN_UP(final_output_bytes, MEM_ARENA_ALIGNMENT);

    // Total size of one "Tray" (SampleChunk data area)
    size_t total_bytes_per_chunk = raw_stride + (complex_stride * 2) + final_stride;

    // Allocate the Big Pool using OS-specific aligned allocation
    // CRITICAL: We now use the dynamically calculated pipeline_num_chunks
    size_t pool_total_size = resources->pipeline_num_chunks * total_bytes_per_chunk;

#ifdef _WIN32
    resources->pipeline_chunk_data_pool = _aligned_malloc(pool_total_size, MEM_ARENA_ALIGNMENT);
#else
    int align_ret = posix_memalign(&resources->pipeline_chunk_data_pool, MEM_ARENA_ALIGNMENT, pool_total_size);
    if (align_ret != 0) {
        resources->pipeline_chunk_data_pool = NULL; // Mark as failed
        log_fatal("posix_memalign failed with error: %d", align_ret);
    }
#endif

    if (!resources->pipeline_chunk_data_pool) {
        log_fatal("Error: Failed to allocate aligned pipeline chunk data pool (%zu bytes).", pool_total_size);
        return false;
    }

    // Allocate metadata structures from the Arena (small objects)
    resources->sample_chunk_pool = (SampleChunk*)mem_arena_alloc(&resources->setup_arena, resources->pipeline_num_chunks * sizeof(SampleChunk), true);
    if (!resources->sample_chunk_pool) return false;

    // Assign pointers within the monolithic pool
    for (size_t i = 0; i < resources->pipeline_num_chunks; ++i) {
        SampleChunk* item = &resources->sample_chunk_pool[i];
        char* chunk_base = (char*)resources->pipeline_chunk_data_pool + (i * total_bytes_per_chunk);

        // Set pointers using the aligned strides
        item->raw_input_data = chunk_base;
        item->complex_sample_buffer_a = (complex_float_t*)(chunk_base + raw_stride);
        item->complex_sample_buffer_b = (complex_float_t*)(chunk_base + raw_stride + complex_stride);
        item->final_output_data = (unsigned char*)(chunk_base + raw_stride + (complex_stride * 2));

        // Set capacities (using the actual usable byte count, not the stride padding)
        item->raw_input_capacity_bytes = raw_input_bytes;
        item->complex_buffer_capacity_samples = resources->pipeline_alloc_size_samples;
        item->final_output_capacity_bytes = final_output_bytes;
        item->input_bytes_per_sample_pair = resources->input_bytes_per_sample_pair;
    }

    // Allocate aux buffers
    resources->sdr_deserializer_buffer_size = PIPELINE_TARGET_BLOCK_SAMPLES * sizeof(short) * COMPLEX_SAMPLE_COMPONENTS * 2; // Double size for safety
    resources->sdr_deserializer_temp_buffer = mem_arena_alloc(&resources->setup_arena, resources->sdr_deserializer_buffer_size, false);
    if (!resources->sdr_deserializer_temp_buffer) return false;

    resources->writer_local_buffer = mem_arena_alloc(&resources->setup_arena, IO_OUTPUT_WRITER_CHUNK_SIZE, false);
    if (!resources->writer_local_buffer) return false;

    return true;
}

bool create_threading_components(AppConfig *config, AppResources *resources) {
    (void)config;
    if (pthread_mutex_init(&resources->progress_mutex, NULL) != 0) {
        return false;
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
        "Output Type", "Sample Type", "Output Rate", "Input Gain", "Output Gain", "Frequency Shift",
        "Resampling", "Output Target", "FIR Filter", "FFT Filter", "Output AGC"
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
    
    fprintf(stderr, " %-*s : %s\n", max_label_len, "I/Q Correction", config->dsp.iq_correction.enable ? "Enabled" : "Disabled");
    fprintf(stderr, " %-*s : %s\n", max_label_len, "DC Block", config->dsp.dc_block.enable ? "Enabled" : "Disabled");


    fprintf(stderr, "--- Output Details ---\n");
    if (resources->selected_output_module_api && resources->selected_output_module_api->get_summary_info) {
        OutputSummaryInfo output_summary;
        memset(&output_summary, 0, sizeof(output_summary));
        resources->selected_output_module_api->get_summary_info(&ctx, &output_summary);
        for (int i = 0; i < output_summary.count; i++) {
            fprintf(stderr, " %-*s : %s\n", max_label_len, output_summary.items[i].label, output_summary.items[i].value);
        }
    }

    const char* sample_type_str = utils_get_format_description_string(config->output.format);
    fprintf(stderr, " %-*s : %s\n", max_label_len, "Sample Type", sample_type_str);

    fprintf(stderr, " %-*s : %.0f Hz\n", max_label_len, "Output Rate", config->output_rate.target_rate);
    
    fprintf(stderr, " %-*s : %.5f\n", max_label_len, "Input Gain", config->dsp.input_gain);

    if (config->dsp.output_gain != 1.0f) {
        fprintf(stderr, " %-*s : %.5f\n", max_label_len, "Output Gain", config->dsp.output_gain);
    }

    if (fabs(resources->nco_shift_hz) > 1e-9) {
        char shift_buf[64];
        snprintf(shift_buf, sizeof(shift_buf), "%+.2f Hz%s", resources->nco_shift_hz, config->dsp.shift_after_resample ? " (Post-Resample)" : "");
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Frequency Shift", shift_buf);
    }

    if (config->dsp.filter.count == 0) {
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
        const char* stage = config->dsp.filter.apply_post_resample ? " (Post-Resample)" : "";
        strncat(filter_buf, "Enabled: ", sizeof(filter_buf) - strlen(filter_buf) - 1);
        for (int i = 0; i < config->dsp.filter.count; i++) {
            char current_filter_desc[128];
            const FilterRequest* req = &config->dsp.filter.requests[i];
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

    if (config->dsp.agc.enable) {
        char agc_buf[128];
        const char* profile_name = "Unknown";
        switch (config->dsp.agc.profile) {
            case AGC_PROFILE_DX:      profile_name = "DX"; break;
            case AGC_PROFILE_LOCAL:   profile_name = "Local"; break;
            case AGC_PROFILE_DIGITAL: profile_name = "Digital"; break;
            default: break;
        }
        
        snprintf(agc_buf, sizeof(agc_buf), "Enabled (Profile: %s, Target: %.2f)", profile_name, config->dsp.agc.target_level);
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Output AGC", agc_buf);
    } else {
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Output AGC", "Disabled");
    }

    fprintf(stderr, " %-*s : %s\n", max_label_len, "Resampling", resources->is_passthrough ? "Disabled (Passthrough Mode)" : "Enabled");

    bool is_file_output = resources->pacing_is_required;
    const char* output_path_for_messages;
#ifdef _WIN32
    output_path_for_messages = config->output.effective_path_utf8;
#else
    output_path_for_messages = config->output.effective_path;
#endif
    fprintf(stderr, " %-*s : %s\n", max_label_len, is_file_output ? "Output File" : "Output Target", is_file_output ? output_path_for_messages : "<stdout>");
    
    // Log the calculated elastic buffer sizes for verification
    log_debug("Pipeline Config: Read Size = %zu samples, Chunk Alloc = %zu samples.", 
              resources->pipeline_read_chunk_size, resources->pipeline_alloc_size_samples);
}

bool prepare_output_stream(struct AppConfig *config, struct AppResources *resources) {
    if (!resources->selected_output_module_api) return false;
    ModuleContext ctx = { .config = config, .resources = resources };
    return resources->selected_output_module_api->initialize(&ctx);
}

bool initialize_application(AppConfig *config, AppResources *resources) {
    resources->config = config;
    ModuleContext ctx = { .config = config, .resources = resources };

    // --- STEP 1: Look up the selected output module ---
    const Module* selected_output_module = module_manager_get_output_module_by_name(config->output.module_name, &resources->setup_arena);
    if (!selected_output_module) {
        log_fatal("Internal error: Could not retrieve selected output module '%s'.", config->output.module_name);
        return false;
    }
    resources->selected_output_module_api = (OutputModuleInterface*)selected_output_module->api;

    // --- STEP 2: SET THE BEHAVIORAL FLAG ---
    resources->pacing_is_required = selected_output_module->requires_output_path;

    log_info("Attempting to initialize the '%s' input module...", config->input.type_name);

    if (!resolve_file_paths(config, resources)) {
        return false;
    }

    if (!resources->selected_input_module_api->initialize(&ctx)) {
        return false;
    }

    if (!calculate_and_validate_resample_ratio(config, resources, &resources->resample_ratio)) {
        return false;
    }

    if (!validate_and_configure_filter_stage(config, resources)) {
        return false;
    }

    if (resources->selected_output_module_api->validate_options) {
        if (!resources->selected_output_module_api->validate_options(config)) {
            return false;
        }
    }

    if (resources->selected_input_module_api->pre_stream_iq_correction) {
        if (!resources->selected_input_module_api->pre_stream_iq_correction(&ctx)) {
            return false;
        }
    }

    if (!allocate_processing_buffers(config, resources, resources->resample_ratio)) {
        return false;
    }

    if (!create_threading_components(config, resources)) {
        return false;
    }

    if (!prepare_output_stream(config, resources)) {
        return false;
    }

    if (resources->pacing_is_required) {
        print_configuration_summary(config, resources);
        fprintf(stderr, "\n"); 
    }

    if (resources->pacing_is_required) {
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

    if (resources->selected_output_module_api && resources->selected_output_module_api->finalize_output) {
        resources->selected_output_module_api->finalize_output(&ctx);
    }

    if (resources->pipeline_chunk_data_pool) {
#ifdef _WIN32
        _aligned_free(resources->pipeline_chunk_data_pool);
#else
        free(resources->pipeline_chunk_data_pool);
#endif
        resources->pipeline_chunk_data_pool = NULL;
    }
 
    if (resources->selected_input_module_api && resources->selected_input_module_api->cleanup) {
        resources->selected_input_module_api->cleanup(&ctx);
    }
}
