/**
 * @file setup.c
 * @brief Declares the high-level functions for application initialization and cleanup.
 */

#include "initialization.h"
#include "sample_convert.h"
#include "constants.h"
#include "platform.h"
#include "utils.h"
#include "log.h"
#include "module.h"
#include "module_registry.h"
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


bool resolve_file_paths(AppConfig *config, AppContext* app) {
    if (!config || !app) return false;

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
        config->input.effective_path = mem_arena_alloc(&app->pipeline.setup_arena, strlen(resolved_input_path) + 1, false);
        if (!config->input.effective_path) return false;
        strcpy(config->input.effective_path, resolved_input_path);
    }

    if (config->output.path_arg) {
        char* path_copy_for_dirname = mem_arena_alloc(&app->pipeline.setup_arena, strlen(config->output.path_arg) + 1, false);
        char* path_copy_for_basename = mem_arena_alloc(&app->pipeline.setup_arena, strlen(config->output.path_arg) + 1, false);
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
        config->output.effective_path = mem_arena_alloc(&app->pipeline.setup_arena, final_len, false);
        if (!config->output.effective_path) return false;

        snprintf(config->output.effective_path, final_len, "%s/%s", resolved_dir_path, base);
    }
#endif
    return true;
}

bool calculate_and_validate_resample_ratio(AppConfig *config, AppContext* app, float *out_ratio) {
    if (!config || !app || !out_ratio) return false;

    // --- Step 1: Handle Smart Default (Missing Rate) ---
    // If the user didn't specify a rate (0), use the hardware/file input rate.
    if (config->output_rate.target_rate <= 0.0) {
        config->output_rate.target_rate = (double)app->module.source_info.sample_rate;
        log_info("No output rate specified. Defaulting to native input rate: %.0f Hz", config->output_rate.target_rate);
    }

    // --- Step 2: Calculate Ratio ---
    double input_rate_d = (double)app->module.source_info.sample_rate;
    float r = (float)(config->output_rate.target_rate / input_rate_d);

    // --- Step 3: Check for Passthrough Conditions ---
    if (config->dsp.raw_passthrough) {
        log_info("Raw Passthrough mode enabled: Bypassing all DSP blocks.");
        app->dsp.is_passthrough = true;
        r = 1.0f; // Force ratio to 1.0 for buffer calcs
    }
    else if (fabs(r - 1.0f) < 1e-6) {
        app->dsp.is_passthrough = true;
        r = 1.0f; // Snap to exact 1.0
    }
    else {
        app->dsp.is_passthrough = false;
        log_info("Resampling enabled: %.10g Hz -> %.10g Hz (Ratio: %.10g)",
                 input_rate_d, config->output_rate.target_rate, r);
    }

    // --- Step 4: Validate Ratio ---
    if (!isfinite(r) || r < MIN_ACCEPTABLE_RATIO || r > MAX_ACCEPTABLE_RATIO) {
        log_fatal("Error: Calculated resampling ratio (%.6f) is invalid or outside acceptable range.", r);
        return false;
    }
    *out_ratio = r;

    if (app->module.source_info.frames > 0) {
        app->stats.expected_total_output_frames = (long long)round((double)app->module.source_info.frames * (double)r);
    } else {
        app->stats.expected_total_output_frames = -1;
    }

    return true;
}

bool validate_and_configure_filter_stage(struct AppConfig *config, struct AppContext* app) {
    config->dsp.filter.apply_post_resample = false;

    // If resampling is disabled (is_passthrough), we don't need to check for post-resample filtering.
    if (config->dsp.filter.count == 0 || app->dsp.is_passthrough || config->dsp.raw_passthrough) {
        return true;
    }

    double input_rate = (double)app->module.source_info.sample_rate;
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
bool allocate_processing_buffers(AppConfig *config, AppContext* app, float resample_ratio) {
    if (!config || !app) return false;

    // 1. Determine the "Fat Pipe" (highest data rate side)
    //    Upsampling (Ratio > 1.0): Output is the Fat Pipe.
    //    Downsampling (Ratio <= 1.0): Input is the Fat Pipe.
    bool upsampling = (resample_ratio > 1.0f);

    size_t target_block_samples = PIPELINE_TARGET_BLOCK_SAMPLES; // 12,288 samples (~192KB)

    // 2. Adjust target for FFT requirements if necessary
    // The filter object does not exist yet. We must estimate requirements based on CONFIG.
    size_t estimated_taps = 0;
    if (config->dsp.filter.args.taps > 0) {
        estimated_taps = config->dsp.filter.args.taps;
    } else if (config->dsp.filter.count > 0) {
        // If taps aren't explicit, assume a worst-case default for sizing
        estimated_taps = FILTER_SAFETY_DEFAULT_TAPS;
    }

    if (estimated_taps > 0) {
        // Calculate the FFT block size logic used by liquid-dsp/filter.c
        size_t req_block_size = 1;
        while (req_block_size < estimated_taps) {
            req_block_size *= 2;
        }
        // Heuristic from filter.c: double it for efficiency
        if (req_block_size < estimated_taps * 2) {
            req_block_size *= 2;
        }

        // If the filter needs huge blocks (e.g. 32k for 15k taps), expand the pipeline chunks.
        if (req_block_size > target_block_samples) {
            target_block_samples = req_block_size;
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
        sample_allocation_count = (size_t)ceil((double)calculated_input_samples * resample_ratio) + PIPELINE_BUFFER_PADDING_SAMPLES;
    }
    else {
        // --- CASE B: DOWNSAMPLING / PASSTHROUGH ---
        // The Input is pinned to the Target.
        calculated_input_samples = target_block_samples;

        // Allocation = Input Size + Safety Margin.
        // (Since output shrinks, the input buffer determines the required memory pool size).
        sample_allocation_count = target_block_samples + PIPELINE_BUFFER_PADDING_SAMPLES;
    }

    // Store the results for runtime usage
    app->pipeline.read_chunk_size = calculated_input_samples;
    app->pipeline.alloc_size_samples = sample_allocation_count;

    // 3. FFT Limit Safety Check
    if (app->pipeline.alloc_size_samples > MAX_ALLOWED_FFT_BLOCK_SIZE) {
        log_fatal("Calculated pipeline buffer size (%zu) exceeds maximum allowed FFT size (%d).",
                  app->pipeline.alloc_size_samples, MAX_ALLOWED_FFT_BLOCK_SIZE);
        return false;
    }

    // Update legacy field used by some filters
    app->pipeline.max_out_samples = (unsigned int)app->pipeline.alloc_size_samples;

    // -------------------------------------------------------------------------
    // 4. Calculate Dynamic Pipeline Depth ("Trays")
    // -------------------------------------------------------------------------
    double input_rate = (double)app->module.source_info.sample_rate;

    // FAIL FAST: If the input rate is unknown or invalid, we cannot safely configure the pipeline.
    if (input_rate <= 0.0) {
        log_fatal("Internal Error: Input sample rate is invalid (%.0f Hz). Cannot calculate buffer depth.", input_rate);
        log_fatal("Please check the input source configuration.");
        return false;
    }

    // How much time does one chunk represent?
    double seconds_per_chunk = (double)app->pipeline.read_chunk_size / input_rate;

    // How many chunks do we need to hit the target duration?
    size_t calculated_chunks = (size_t)(PIPELINE_TARGET_BUFFER_DURATION_SEC / seconds_per_chunk);

    // Apply Sanity Clamps
    if (calculated_chunks < PIPELINE_MIN_CHUNKS) calculated_chunks = PIPELINE_MIN_CHUNKS;
    if (calculated_chunks > PIPELINE_MAX_CHUNKS) calculated_chunks = PIPELINE_MAX_CHUNKS;

    app->pipeline.num_chunks = calculated_chunks;

    log_info("Pipeline Sizing: Read=%zu samples, Alloc=%zu samples, Depth=%zu chunks (%.2f sec buffer at %.0f Hz)",
              app->pipeline.read_chunk_size,
              app->pipeline.alloc_size_samples,
              app->pipeline.num_chunks,
              app->pipeline.num_chunks * seconds_per_chunk,
              input_rate);

    // --- MEMORY ALLOCATION WITH ALIGNMENT ---

    // Calculate raw byte sizes
    size_t raw_input_bytes = app->pipeline.alloc_size_samples * app->module.input_bytes_per_sample_pair;
    size_t complex_bytes = app->pipeline.alloc_size_samples * sizeof(ComplexFloat);
    app->module.output_bytes_per_sample_pair = sample_convert_bytes_per_sample(config->output.format);
    size_t final_output_bytes = app->pipeline.alloc_size_samples * app->module.output_bytes_per_sample_pair;

    // Calculate Strides (Aligned to 32 bytes)
    size_t raw_stride = ALIGN_UP(raw_input_bytes, MEM_ARENA_ALIGNMENT);
    size_t complex_stride = ALIGN_UP(complex_bytes, MEM_ARENA_ALIGNMENT);
    size_t final_stride = ALIGN_UP(final_output_bytes, MEM_ARENA_ALIGNMENT);

    // Total size of one "Tray" (SampleChunk data area)
    size_t total_bytes_per_chunk = raw_stride + (complex_stride * 2) + final_stride;

    // Allocate the Big Pool using OS-specific aligned allocation
    // CRITICAL: We now use the dynamically calculated pipeline_num_chunks
    size_t pool_total_size = app->pipeline.num_chunks * total_bytes_per_chunk;

    size_t aligned_pool_total_size = ALIGN_UP(pool_total_size, MEM_ARENA_ALIGNMENT);
    app->pipeline.chunk_data_pool = aligned_alloc(MEM_ARENA_ALIGNMENT, aligned_pool_total_size);

    if (!app->pipeline.chunk_data_pool) {
        log_fatal("Error: Failed to allocate aligned pipeline chunk data pool (%zu bytes).", pool_total_size);
        return false;
    }

    // Allocate metadata structures from the Arena (small objects)
    app->pipeline.sample_chunk_pool = (SampleChunk*)mem_arena_alloc(&app->pipeline.setup_arena, app->pipeline.num_chunks * sizeof(SampleChunk), true);
    if (!app->pipeline.sample_chunk_pool) return false;

    // Assign pointers within the monolithic pool
    for (size_t i = 0; i < app->pipeline.num_chunks; ++i) {
        SampleChunk* item = &app->pipeline.sample_chunk_pool[i];
        char* chunk_base = (char*)app->pipeline.chunk_data_pool + (i * total_bytes_per_chunk);

        // Set pointers using the aligned strides
        item->raw_input_data = chunk_base;
        item->complex_sample_buffer_a = (ComplexFloat*)(chunk_base + raw_stride);
        item->complex_sample_buffer_b = (ComplexFloat*)(chunk_base + raw_stride + complex_stride);
        item->final_output_data = (unsigned char*)(chunk_base + raw_stride + (complex_stride * 2));

        // Set capacities (using the actual usable byte count, not the stride padding)
        item->raw_input_capacity_bytes = raw_input_bytes;
        item->complex_buffer_capacity_samples = app->pipeline.alloc_size_samples;
        item->final_output_capacity_bytes = final_output_bytes;
        item->input_bytes_per_sample_pair = app->module.input_bytes_per_sample_pair;
    }

    // Allocate aux buffers
    app->pipeline.sdr_deserializer_buffer_size = PIPELINE_TARGET_BLOCK_SAMPLES * sizeof(short) * COMPLEX_SAMPLE_COMPONENTS * 2; // Double size for safety
    app->pipeline.sdr_deserializer_temp_buffer = mem_arena_alloc(&app->pipeline.setup_arena, app->pipeline.sdr_deserializer_buffer_size, false);
    if (!app->pipeline.sdr_deserializer_temp_buffer) return false;

    app->pipeline.writer_local_buffer = mem_arena_alloc(&app->pipeline.setup_arena, OUTPUT_WRITER_CHUNK_SIZE, false);
    if (!app->pipeline.writer_local_buffer) return false;

    // -------------------------------------------------------------------------
    // 5. Calculate Dynamic Ring Buffer Sizes
    // -------------------------------------------------------------------------

    // Calculate input buffer size (for buffered mode)
    if (app->pipeline_mode != PIPELINE_MODE_FILE_PROCESSING) {
        size_t input_buffer_bytes = (size_t)(
            input_rate *
            INPUT_BUFFER_DURATION_SEC *
            app->module.input_bytes_per_sample_pair
        );

        if (input_buffer_bytes < INPUT_BUFFER_MIN_BYTES)
            input_buffer_bytes = INPUT_BUFFER_MIN_BYTES;
        if (input_buffer_bytes > INPUT_BUFFER_MAX_BYTES)
            input_buffer_bytes = INPUT_BUFFER_MAX_BYTES;

        app->pipeline.input_buffer_size = input_buffer_bytes;

        log_info("Input Buffer: Allocating %zu bytes (%.2f sec capacity) at %.0f Hz.",
                 input_buffer_bytes,
                 INPUT_BUFFER_DURATION_SEC,
                 input_rate);
    }

    // Calculate output writer buffer size (for file output)
    if (app->module.pacing_is_required) {
        size_t writer_buffer_bytes = (size_t)(
            config->output_rate.target_rate *
            OUTPUT_WRITER_BUFFER_DURATION_SEC *
            app->module.output_bytes_per_sample_pair
        );

        if (writer_buffer_bytes < OUTPUT_WRITER_BUFFER_MIN_BYTES)
            writer_buffer_bytes = OUTPUT_WRITER_BUFFER_MIN_BYTES;
        if (writer_buffer_bytes > OUTPUT_WRITER_BUFFER_MAX_BYTES)
            writer_buffer_bytes = OUTPUT_WRITER_BUFFER_MAX_BYTES;

        app->pipeline.output_writer_buffer_size = writer_buffer_bytes;

        log_info("Output Writer Buffer: %zu MB (%.1f seconds at %.0f Hz)",
                 writer_buffer_bytes / (1024 * 1024),
                 OUTPUT_WRITER_BUFFER_DURATION_SEC,
                 config->output_rate.target_rate);
    }

    return true;
}

bool create_threading_components(AppConfig *config, AppContext* app) {
    (void)config;
    if (pthread_mutex_init(&app->stats.mutex, NULL) != 0) {
        return false;
    }
    return true;
}

void print_configuration_summary(const AppConfig *config, const AppContext* app) {
    if (!config || !app || !app->module.input_api) return;

    InputSummaryInfo summary_info;
    memset(&summary_info, 0, sizeof(InputSummaryInfo));
    const ModuleContext ctx = { .config = config, .app = (AppContext*)app };
    app->module.input_api->get_summary_info(&ctx, &summary_info);

    int max_label_len = 0;
    if (summary_info.count > 0) {
        for (int i = 0; i < summary_info.count; i++) {
            int len = (int)strlen(summary_info.items[i].label);
            if (len > max_label_len) {
                max_label_len = len;
            }
        }
    }

    // Check our dynamic items if offset is active to ensure alignment
    bool use_offset_display = (fabs(config->sdr_general.frequency_offset_hz) > 1e-9);

    if (use_offset_display) {
        const char* offset_labels[] = { "Actual Frequency", "Frequency Offset", "Tuned Frequency" };
        for (int i = 0; i < 3; i++) {
            int len = (int)strlen(offset_labels[i]);
            if (len > max_label_len) max_label_len = len;
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

            // Intercept "RF Frequency" ONLY if we are using an offset
            if (use_offset_display && strcmp(summary_info.items[i].label, "RF Frequency") == 0) {

                // MATH: User Target = Hardware - Offset
                double user_target_hz = config->sdr_general.rf_freq_hz - config->sdr_general.frequency_offset_hz;

                // 1. Source (The Signal / User Intent)
                fprintf(stderr, " %-*s : %.0f Hz\n", max_label_len, "Actual Frequency", user_target_hz);

                // 2. Modifier (The Converter Offset)
                fprintf(stderr, " %-*s : %+.0f Hz\n", max_label_len, "Frequency Offset", config->sdr_general.frequency_offset_hz);

                // 3. Result (The Hardware Reality)
                fprintf(stderr, " %-*s : %s\n", max_label_len, "Tuned Frequency", summary_info.items[i].value);

            } else {
                // Standard Print
                fprintf(stderr, " %-*s : %s\n", max_label_len, summary_info.items[i].label, summary_info.items[i].value);
            }
        }
    }

    fprintf(stderr, " %-*s : %s\n", max_label_len, "I/Q Correction", config->dsp.iq_correction.enable ? "Enabled" : "Disabled");
    fprintf(stderr, " %-*s : %s\n", max_label_len, "DC Block", config->dsp.dc_block.enable ? "Enabled" : "Disabled");


    fprintf(stderr, "--- Output Details ---\n");
    if (app->module.output_api && app->module.output_api->get_summary_info) {
        OutputSummaryInfo output_summary;
        memset(&output_summary, 0, sizeof(output_summary));
        app->module.output_api->get_summary_info(&ctx, &output_summary);
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

    if (fabs(app->dsp.nco_shift_hz) > 1e-9) {
        char shift_buf[64];
        snprintf(shift_buf, sizeof(shift_buf), "%+.2f Hz%s", app->dsp.nco_shift_hz, config->dsp.shift_after_resample ? " (Post-Resample)" : "");
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Frequency Shift", shift_buf);
    }

    if (config->dsp.filter.count == 0) {
        fprintf(stderr, " %-*s : %s\n", max_label_len, "Filter", "Disabled");
    } else {
        const char* filter_label;
        switch (app->dsp.filter.type_actual) {
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

    fprintf(stderr, " %-*s : %s\n", max_label_len, "Resampling", app->dsp.is_passthrough ? "Disabled (Passthrough Mode)" : "Enabled");

    bool is_file_output = app->module.pacing_is_required;
    const char* output_path_for_messages;
#ifdef _WIN32
    output_path_for_messages = config->output.effective_path_utf8;
#else
    output_path_for_messages = config->output.effective_path;
#endif
    fprintf(stderr, " %-*s : %s\n", max_label_len, is_file_output ? "Output File" : "Output Target", is_file_output ? output_path_for_messages : "<stdout>");

    // Log the calculated elastic buffer sizes for verification
    log_debug("Pipeline Config: Read Size = %zu samples, Chunk Alloc = %zu samples.",
              app->pipeline.read_chunk_size, app->pipeline.alloc_size_samples);
}

bool prepare_output_stream(struct AppConfig *config, struct AppContext* app) {
    if (!app->module.output_api) return false;
    ModuleContext ctx = { .config = config, .app = app };
    return app->module.output_api->initialize(&ctx);
}

bool initialize_application(AppConfig *config, AppContext* app) {
    app->config = config;
    app->dsp.config = config;
    ModuleContext ctx = { .config = config, .app = app };

    // --- STEP 1: Look up the selected output module ---
    const Module* selected_output_module = module_get(config->output.module_name, MODULE_TYPE_OUTPUT, &app->pipeline.setup_arena);
    if (!selected_output_module) {
        log_fatal("Internal error: Could not retrieve selected output module '%s'.", config->output.module_name);
        return false;
    }
    app->module.output_api = (OutputModuleInterface*)selected_output_module->api;

    // --- STEP 2: SET THE BEHAVIORAL FLAG ---
    app->module.pacing_is_required = selected_output_module->requires_output_path;

    log_info("Attempting to initialize the '%s' input module...", config->input.type_name);

    if (!resolve_file_paths(config, app)) {
        return false;
    }

    if (!app->module.input_api->initialize(&ctx)) {
        return false;
    }

    // --- REMOVED: Output Option Validation (Moved to cli.c) ---
    // This ensures validation happens before expensive input initialization.

    if (!calculate_and_validate_resample_ratio(config, app, &app->dsp.resample_ratio)) {
        return false;
    }

    if (!validate_and_configure_filter_stage(config, app)) {
        return false;
    }

    if (app->module.input_api->pre_stream_iq_correction) {
        if (!app->module.input_api->pre_stream_iq_correction(&ctx)) {
            return false;
        }
    }

    if (!allocate_processing_buffers(config, app, app->dsp.resample_ratio)) {
        return false;
    }

    if (!create_threading_components(config, app)) {
        return false;
    }

    if (!prepare_output_stream(config, app)) {
        return false;
    }

    if (app->module.pacing_is_required) {
        print_configuration_summary(config, app);
        fprintf(stderr, "\n");
    }

    if (app->module.pacing_is_required) {
        bool source_has_known_length = app->module.input_api->has_known_length();
        if (!source_has_known_length) {
            log_info("Starting SDR capture...");
        } else {
            log_info("Starting file processing...");
        }
    }

    return true;
}

void cleanup_application(AppConfig *config, AppContext* app) {
    if (!app) return;
    ModuleContext ctx = { .config = config, .app = app };

    if (app->module.output_api && app->module.output_api->cleanup) {
        app->module.output_api->cleanup(&ctx);
    }

    if (app->pipeline.chunk_data_pool) {
        aligned_free(app->pipeline.chunk_data_pool);
        app->pipeline.chunk_data_pool = NULL;
    }

    if (app->module.input_api && app->module.input_api->cleanup) {
        app->module.input_api->cleanup(&ctx);
    }
}
