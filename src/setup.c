#include "setup.h"
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
#include "resampler.h"
#include "memory_arena.h"
#include "queue.h"
#include "file_write_buffer.h"
#include "app_context.h"
#include "thread_manager.h" // For threads_destroy_queues
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

// --- Forward declarations for static functions ---
static bool create_dc_blocker(AppConfig *config, AppResources *resources);
static bool create_iq_corrector(AppConfig *config, AppResources *resources);
static bool create_frequency_shifter(AppConfig *config, AppResources *resources);
static bool create_filter(AppConfig *config, AppResources *resources);

// --- START: New helpers for timed initialization ---

// Context to share state between the main thread and the initializer thread.
typedef struct {
    InputSourceContext* input_ctx;
    pthread_mutex_t mutex;
    bool is_complete;
    bool success;
} InitializerContext;

// The initializer thread's only job is to run the blocking function and set a flag.
static void* initializer_thread_func(void* arg) {
    InitializerContext* ctx = (InitializerContext*)arg;

    // Run the potentially blocking function
    bool result = ctx->input_ctx->resources->selected_input_ops->initialize(ctx->input_ctx);

    // Lock, update state, and unlock
    pthread_mutex_lock(&ctx->mutex);
    ctx->success = result;
    ctx->is_complete = true;
    pthread_mutex_unlock(&ctx->mutex);

    return NULL;
}

// --- END: New helpers for timed initialization ---


static bool create_dc_blocker(AppConfig *config, AppResources *resources) {
    if (!config->dc_block.enable) return true;
    return dc_block_create(config, resources);
}

static bool create_iq_corrector(AppConfig *config, AppResources *resources) {
    if (!config->iq_correction.enable) return true;
    return iq_correct_init(config, resources, &resources->setup_arena);
}

static bool create_frequency_shifter(AppConfig *config, AppResources *resources) {
    // If a shift hasn't already been calculated by a module (like WAV),
    // then check for the generic manual shift option.
    if (resources->nco_shift_hz == 0.0 && config->freq_shift_hz_arg != 0.0f) {
        resources->nco_shift_hz = (double)config->freq_shift_hz_arg;
    }

    // Now that the final shift value is resolved, validate dependent options.
    if (config->shift_after_resample && fabs(resources->nco_shift_hz) < 1e-9) {
        log_fatal("Option --shift-after-resample was used, but no effective frequency shift was requested or calculated.");
        return false;
    }

    // Now, create the NCOs with the final, resolved value.
    return freq_shift_create_ncos(config, resources);
}

static bool create_filter(AppConfig *config, AppResources *resources) {
    return filter_create(config, resources, &resources->setup_arena);
}


bool resolve_file_paths(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

#ifdef _WIN32
    // Writes directly into the fixed-size buffers in AppConfig
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
        // Since dirname() and basename() can modify the input string, we must work on copies.
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

        // Allocate space for the final, combined path.
        size_t final_len = strlen(resolved_dir_path) + 1 + strlen(base) + 1;
        config->effective_output_filename = mem_arena_alloc(&resources->setup_arena, final_len, false);
        if (!config->effective_output_filename) return false;

        // Safely combine the resolved directory and the original basename.
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

bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    size_t max_pre_resample_chunk_size = PIPELINE_CHUNK_BASE_SAMPLES;
    bool is_pre_fft_filter = (resources->user_filter_object && !config->apply_user_filter_post_resample &&
                             (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC ||
                              resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC));

    if (is_pre_fft_filter) {
        if (resources->user_filter_block_size > max_pre_resample_chunk_size) {
            max_pre_resample_chunk_size = resources->user_filter_block_size;
        }
    }

    size_t resampler_output_capacity = (size_t)ceil((double)max_pre_resample_chunk_size * fmax(1.0, (double)resample_ratio)) + RESAMPLER_OUTPUT_SAFETY_MARGIN;
    size_t required_capacity = (max_pre_resample_chunk_size > resampler_output_capacity) ? max_pre_resample_chunk_size : resampler_output_capacity;

    bool is_post_fft_filter = (resources->user_filter_object && config->apply_user_filter_post_resample &&
                              (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC ||
                               resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC));

    if (is_post_fft_filter) {
        if (resources->user_filter_block_size > required_capacity) {
            required_capacity = resources->user_filter_block_size;
        }
    }

    if (required_capacity > MAX_ALLOWED_FFT_BLOCK_SIZE) {
        log_fatal("Error: Pipeline requires a buffer size (%zu) that exceeds the maximum allowed size (%d).",
                  required_capacity, MAX_ALLOWED_FFT_BLOCK_SIZE);
        return false;
    }

    resources->max_out_samples = required_capacity;
    log_debug("Calculated required processing buffer capacity: %u samples.", resources->max_out_samples);

    size_t raw_input_bytes_per_chunk = PIPELINE_CHUNK_BASE_SAMPLES * resources->input_bytes_per_sample_pair;
    size_t complex_bytes_per_chunk = resources->max_out_samples * sizeof(complex_float_t);
    resources->output_bytes_per_sample_pair = get_bytes_per_sample(config->output_format);
    size_t final_output_bytes_per_chunk = resources->max_out_samples * resources->output_bytes_per_sample_pair;

    size_t total_bytes_per_chunk = raw_input_bytes_per_chunk +
                                   (complex_bytes_per_chunk * 2) + // ping-pong complex buffers
                                   final_output_bytes_per_chunk;

    resources->pipeline_chunk_data_pool = malloc(PIPELINE_NUM_CHUNKS * total_bytes_per_chunk);
    if (!resources->pipeline_chunk_data_pool) {
        log_fatal("Error: Failed to allocate the main pipeline chunk data pool.");
        return false;
    }

    resources->sample_chunk_pool = (SampleChunk*)mem_arena_alloc(&resources->setup_arena, PIPELINE_NUM_CHUNKS * sizeof(SampleChunk), true);
    if (!resources->sample_chunk_pool) return false;

    // Allocate the de-interleaving buffer. It must be large enough to hold
    // both planes (I and Q) of a full sample chunk.
    resources->sdr_deserializer_buffer_size = PIPELINE_CHUNK_BASE_SAMPLES * sizeof(short) * COMPLEX_SAMPLE_COMPONENTS;
    resources->sdr_deserializer_temp_buffer = mem_arena_alloc(&resources->setup_arena, resources->sdr_deserializer_buffer_size, false);
    if (!resources->sdr_deserializer_temp_buffer) return false;

    resources->writer_local_buffer = mem_arena_alloc(&resources->setup_arena, IO_FILE_WRITER_CHUNK_SIZE, false);
    if (!resources->writer_local_buffer) return false;

    for (size_t i = 0; i < PIPELINE_NUM_CHUNKS; ++i) {
        SampleChunk* item = &resources->sample_chunk_pool[i];
        char* chunk_base = (char*)resources->pipeline_chunk_data_pool + i * total_bytes_per_chunk;

        item->raw_input_data = chunk_base;
        item->complex_sample_buffer_a = (complex_float_t*)(chunk_base + raw_input_bytes_per_chunk);
        item->complex_sample_buffer_b = (complex_float_t*)(chunk_base + raw_input_bytes_per_chunk + complex_bytes_per_chunk);
        item->final_output_data = (unsigned char*)(chunk_base + raw_input_bytes_per_chunk + (complex_bytes_per_chunk * 2));

        item->raw_input_capacity_bytes = raw_input_bytes_per_chunk;
        item->complex_buffer_capacity_samples = resources->max_out_samples;
        item->final_output_capacity_bytes = final_output_bytes_per_chunk;
        item->input_bytes_per_sample_pair = resources->input_bytes_per_sample_pair;
    }

    return true;
}

bool create_threading_components(AppConfig *config, AppResources *resources) {
    MemoryArena* arena = &resources->setup_arena;
    resources->free_sample_chunk_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);

    if (!queue_init(resources->free_sample_chunk_queue, PIPELINE_NUM_CHUNKS, arena)) {
        return false;
    }

    if (config->iq_correction.enable) {
        resources->iq_optimization_data_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!queue_init(resources->iq_optimization_data_queue, PIPELINE_NUM_CHUNKS, arena)) return false;
    } else {
        resources->iq_optimization_data_queue = NULL;
    }

    for (size_t i = 0; i < PIPELINE_NUM_CHUNKS; ++i) {
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

    if (!file_writer_init(&resources->writer_ctx, config)) {
        return false;
    }

    if (!resources->writer_ctx.ops.open(&resources->writer_ctx, config, resources, &resources->setup_arena)) {
        return false;
    }
    return true;
}

bool initialize_application(AppConfig *config, AppResources *resources) {
    bool success = false;
    resources->config = config;
    resources->lifecycle_state = LIFECYCLE_STATE_START;
    InputSourceContext ctx = { .config = config, .resources = resources };
    float resample_ratio = 0.0f;

    // STEP 1: Determine pipeline mode
    bool is_sdr = is_sdr_input(config->input_type_str, &resources->setup_arena);
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

    // STEP 2: Initialize hardware and file handles
    if (!resolve_file_paths(config, resources)) {
        goto cleanup;
    }

    if (is_sdr) {
        log_info("Attempting to initialize the %s device...", config->input_type_str);

        pthread_t initializer_thread;
        InitializerContext init_thread_ctx;
        init_thread_ctx.input_ctx = &ctx;
        init_thread_ctx.is_complete = false;
        init_thread_ctx.success = false;
        pthread_mutex_init(&init_thread_ctx.mutex, NULL);
        
        if (pthread_create(&initializer_thread, NULL, initializer_thread_func, &init_thread_ctx) != 0) {
            log_fatal("Fatal: Failed to create the device initializer thread.");
            pthread_mutex_destroy(&init_thread_ctx.mutex);
            goto cleanup;
        }

        double start_time = get_monotonic_time_sec();
        double timeout_sec = (double)SDR_INITIALIZE_TIMEOUT_MS / 1000.0;
        bool timed_out = false;
        
        while (true) {
            pthread_mutex_lock(&init_thread_ctx.mutex);
            bool is_done = init_thread_ctx.is_complete;
            pthread_mutex_unlock(&init_thread_ctx.mutex);

            if (is_done) {
                break; // The initializer thread has finished.
            }

            if (get_monotonic_time_sec() - start_time >= timeout_sec) {
                timed_out = true;
                break; // Timeout has been reached.
            }
            
            #ifdef _WIN32
                Sleep(50); // 50 ms
            #else
                struct timespec sleep_time = {0, 50000000L}; // 50 ms
                nanosleep(&sleep_time, NULL);
            #endif
        }

        if (timed_out) {
            fprintf(stderr, "\nFATAL: SDR Initialization Timed Out.\n");
            fprintf(stderr, "FATAL: No response from the %s device within %d seconds.\n",
                    config->input_type_str, SDR_INITIALIZE_TIMEOUT_MS / 1000);
            fprintf(stderr, "FATAL: The SDR driver has likely hung due to a crash or device removal.\n");
            fprintf(stderr, "FATAL: Forcing application exit.\n");
            fflush(stderr);
            pthread_detach(initializer_thread);
            exit(EXIT_FAILURE);
        }
        
        pthread_join(initializer_thread, NULL);
        pthread_mutex_destroy(&init_thread_ctx.mutex);

        if (!init_thread_ctx.success) {
            log_debug("Device initialization thread finished but reported failure.");
            goto cleanup;
        }

    } else {
        if (!resources->selected_input_ops->initialize(&ctx)) {
            goto cleanup;
        }
    }
    
    resources->lifecycle_state = LIFECYCLE_STATE_INPUT_INITIALIZED;

    // STEP 3: Perform initial calculations and validations
    if (!calculate_and_validate_resample_ratio(config, resources, &resample_ratio)) {
        goto cleanup;
    }
 
    // STEP 4: Initialize all individual DSP components in a consistent, logical order
    if (!create_dc_blocker(config, resources)) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_DC_BLOCK_CREATED;

    if (!create_iq_corrector(config, resources)) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_IQ_CORRECTOR_CREATED;

    if (!create_frequency_shifter(config, resources)) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_FREQ_SHIFTER_CREATED;

    resources->resampler = create_resampler(config, resources, resample_ratio);
    if (!resources->resampler && !resources->is_passthrough) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_RESAMPLER_CREATED;

    if (!create_filter(config, resources)) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_FILTER_CREATED;

    if (resources->selected_input_ops->pre_stream_iq_correction) {
        if (!resources->selected_input_ops->pre_stream_iq_correction(&ctx)) {
            goto cleanup;
        }
    }

    // Conditionally allocate FFT remainder buffers from the arena if needed.
    if (resources->user_filter_object &&
       (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC ||
        resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC))
    {
        if (config->apply_user_filter_post_resample) {
            resources->post_fft_remainder_buffer = (complex_float_t*)mem_arena_alloc(
                &resources->setup_arena,
                resources->user_filter_block_size * sizeof(complex_float_t),
                false
            );
            resources->post_fft_remainder_len = 0;
            if (!resources->post_fft_remainder_buffer) {
                goto cleanup;
            }
        } else {
            resources->pre_fft_remainder_buffer = (complex_float_t*)mem_arena_alloc(
                &resources->setup_arena,
                resources->user_filter_block_size * sizeof(complex_float_t),
                false
            );
            resources->pre_fft_remainder_len = 0;
            if (!resources->pre_fft_remainder_buffer) {
                goto cleanup;
            }
        }
    }
 
    // STEP 5: Allocate memory pools and threading components
    if (!allocate_processing_buffers(config, resources, resample_ratio)) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_BUFFERS_ALLOCATED;

    if (!create_threading_components(config, resources)) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_THREADS_CREATED;

    // STEP 6: Create large I/O ring buffers (if needed)
    if (resources->pipeline_mode == PIPELINE_MODE_BUFFERED_SDR) {
        resources->sdr_input_buffer = file_write_buffer_create(IO_SDR_INPUT_BUFFER_BYTES);
        if (!resources->sdr_input_buffer) {
            log_fatal("Failed to create SDR input buffer for buffered mode.");
            goto cleanup;
        }
    }
    if (!config->output_to_stdout) {
        resources->file_write_buffer = file_write_buffer_create(IO_FILE_WRITER_BUFFER_BYTES);
        if (!resources->file_write_buffer) {
            log_fatal("Failed to create I/O output buffer.");
            goto cleanup;
        }
    } else {
        resources->file_write_buffer = NULL;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_IO_BUFFERS_CREATED;

    // STEP 7: Final checks, summary print, and output stream preparation
    if (!config->output_to_stdout) {
        print_configuration_summary(config, resources);

        if (fabs(resources->nco_shift_hz) > 1e-9) {
            double rate_for_shift_check = config->shift_after_resample ? config->target_rate : (double)resources->source_info.samplerate;
            if (!utils_check_nyquist_warning(fabs(resources->nco_shift_hz), rate_for_shift_check, "Frequency Shift")) {
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
                    default: break;
                }
                if (context && !utils_check_nyquist_warning(freq_to_check, rate_for_filter_check, context)) {
                    goto cleanup;
                }
            }
        }
    }

    if (!prepare_output_stream(config, resources)) {
        goto cleanup;
    }
    resources->lifecycle_state = LIFECYCLE_STATE_OUTPUT_STREAM_OPEN;

    success = true;
    resources->lifecycle_state = LIFECYCLE_STATE_FULLY_INITIALIZED;

cleanup:
    if (!success) {
        // Let main() handle the destruction of the arena.
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

    switch (resources->lifecycle_state) {
        case LIFECYCLE_STATE_FULLY_INITIALIZED:
        case LIFECYCLE_STATE_OUTPUT_STREAM_OPEN:
            if (resources->writer_ctx.ops.close) {
                resources->writer_ctx.ops.close(&resources->writer_ctx);
            }
            if (resources->writer_ctx.ops.get_total_bytes_written) {
                resources->final_output_size_bytes = resources->writer_ctx.ops.get_total_bytes_written(&resources->writer_ctx);
            }
            // fall-through
        case LIFECYCLE_STATE_IO_BUFFERS_CREATED:
            if (resources->sdr_input_buffer) {
                file_write_buffer_destroy(resources->sdr_input_buffer);
                resources->sdr_input_buffer = NULL;
            }
            if (resources->file_write_buffer) {
                file_write_buffer_destroy(resources->file_write_buffer);
                resources->file_write_buffer = NULL;
            }
            // fall-through
        case LIFECYCLE_STATE_THREADS_CREATED:
            // Queue destruction is now handled by the thread manager
            threads_destroy_queues(resources);
            pthread_mutex_destroy(&resources->progress_mutex);
            // fall-through
        case LIFECYCLE_STATE_BUFFERS_ALLOCATED:
            if (resources->pipeline_chunk_data_pool) {
                free(resources->pipeline_chunk_data_pool);
                resources->pipeline_chunk_data_pool = NULL;
            }
            // fall-through
        case LIFECYCLE_STATE_FILTER_CREATED:
            filter_destroy(resources);
            // fall-through
        case LIFECYCLE_STATE_RESAMPLER_CREATED:
            destroy_resampler(resources->resampler);
            resources->resampler = NULL;
            // fall-through
        case LIFECYCLE_STATE_FREQ_SHIFTER_CREATED:
            freq_shift_destroy_ncos(resources);
            // fall-through
        case LIFECYCLE_STATE_IQ_CORRECTOR_CREATED:
            iq_correct_destroy(resources);
            // fall-through
        case LIFECYCLE_STATE_DC_BLOCK_CREATED:
            dc_block_destroy(resources);
            // fall-through
        case LIFECYCLE_STATE_INPUT_INITIALIZED:
            if (resources->selected_input_ops && resources->selected_input_ops->cleanup) {
                resources->selected_input_ops->cleanup(&ctx);
            }
            // fall-through
        case LIFECYCLE_STATE_START:
            // No resources to clean up at this stage
            break;
    }
}
