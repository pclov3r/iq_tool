/**
 * @file pipeline.c
 * @brief Implements the creation, execution, and destruction of the application's DSP pipeline.
 *
 * This module is the central orchestrator for the application's concurrent processing.
 * It contains the master `pipeline_run` function, as well as the private implementations
 * for all the pipeline's concurrent stages and utility threads.
 */

#include "pipeline.h"
#include "pipeline_stages.h"
#include "pipeline_context.h"
#include "thread_manager.h"
#include "utility_threads.h"
#include "input_common.h"
#include "constants.h"
#include "app_context.h"
#include "utilities.h"
#include "platform.h" // Added for thread priority abstraction
#include "signal_handler.h"
#include "log.h"
#include "module_registry.h"
#include "pre_processor.h"
#include "post_processor.h"
#include "dc_block.h"
#include "iq_correction.h"
#include "frequency_shift.h"
#include "resampler.h"
#include "filter.h"
#include "agc.h" // Added for Output AGC
#include "sample_convert.h"
#include "sample_format_table.h"
#include "queue.h"
#include "ring_buffer.h"
#include "packet_serializer.h"
#include "wait_event.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <time.h>
#endif

// Helper macro to align a size up to the next power of 2 boundary
#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))

// --- Private Function Prototypes for Setup Helpers ---
static bool _init_queues_and_buffers(AppConfig* config, AppContext* app);
static void _destroy_queues_and_buffers(AppContext* app);
static bool _create_dsp_components(AppConfig* config, AppContext* app, float resample_ratio);
static void _destroy_dsp_components(AppContext* app);

/**
 * @brief Creates, runs, and waits for the entire processing pipeline to complete.
 *
 * This is the main high-level function that encapsulates the entire pipeline lifecycle.
 * It handles the creation of all DSP objects and queues, spawns all necessary threads
 * using the thread manager, waits for them to finish, and then cleans up all
 * pipeline-specific app.
 *
 * @param context A pointer to the PipelineContext, containing the application config and app.
 * @return true if the pipeline ran and shut down cleanly, false if there was a setup or execution error.
 */
static bool calculate_and_validate_resample_ratio(AppConfig *config, AppContext* app, float *out_ratio) {
    if (!config || !app || !out_ratio) return false;

    // --- Step 1: Determine Target Rate ---
    double target_rate_hz = 0.0;
    if (config->output.payload == PAYLOAD_AUDIO) {
        target_rate_hz = config->baseband_sample_rate.rate_hz;
    } else {
        target_rate_hz = config->output_sample_rate.rate_hz;
    }

    // --- Step 2: Handle Smart Default (Missing Rate) ---
    // If the user didn't specify a rate (0), use the hardware/file input rate.
    if (target_rate_hz <= 0.0) {
        target_rate_hz = (double)app->module.source_info.sample_rate;
        log_info("No explicit pipeline output rate specified. Defaulting to native input rate: %.15g Hz", target_rate_hz);
    }

    // Set the unified pipeline sample rate, format, gain, and agc
    app->dsp.pipeline_sample_rate_hz = target_rate_hz;
    app->dsp.pipeline_sample_format = (config->output.payload == PAYLOAD_AUDIO) ? config->baseband_sample_format.format : config->output.sample_format;
    app->dsp.pipeline_gain = (config->output.payload == PAYLOAD_AUDIO) ? config->dsp.baseband_gain : config->dsp.output_gain;
    app->dsp.pipeline_agc = (config->output.payload == PAYLOAD_AUDIO) ? config->dsp.baseband_agc : config->dsp.output_agc;

    // --- Step 3: Calculate Ratio ---
    double input_rate_d = (double)app->module.source_info.sample_rate;
    float r = (float)(app->dsp.pipeline_sample_rate_hz / input_rate_d);

    // --- Step 4: Check for Passthrough Conditions ---
    if (config->dsp.raw_passthrough) {
        log_info("Raw Passthrough mode enabled: Bypassing all DSP blocks.");
        app->dsp.bypass_resampler = true;
        r = 1.0f; // Force ratio to 1.0 for buffer calcs
    }
    else if (fabs(r - 1.0f) < 1e-6) {
        app->dsp.bypass_resampler = true;
        r = 1.0f; // Snap to exact 1.0
    }
    else {
        app->dsp.bypass_resampler = false;
        log_info("Resampling enabled: %.15g Hz -> %.15g Hz (Ratio: %.15g)",
                 input_rate_d, app->dsp.pipeline_sample_rate_hz, r);
    }

    // --- Step 4: Validate Ratio ---
    if (!isfinite(r) || r < MIN_ACCEPTABLE_RATIO || r > MAX_ACCEPTABLE_RATIO) {
        log_error("Error: Calculated resampling ratio (%.6f) is invalid or outside acceptable range.", r);
        return false;
    }
    *out_ratio = r;

    if (app->module.source_info.frames > 0) {
        atomic_store_explicit(&app->stats.expected_total_output_frames, (long long)round((double)app->module.source_info.frames * (double)r), memory_order_relaxed);
    } else {
        app->stats.expected_total_output_frames = -1;
    }

    return true;
}

static bool allocate_processing_buffers(AppConfig *config, AppContext* app, float resample_ratio) {
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

    size_t req_block_size = 0;
    if (estimated_taps > 0) {
        // Calculate the FFT block size logic used by liquid-dsp/filter.c
        req_block_size = 1;
        while (req_block_size < estimated_taps) {
            req_block_size *= 2;
        }
        // Heuristic from filter.c: double it for efficiency
        if (req_block_size < estimated_taps * 2) {
            req_block_size *= 2;
        }

        // If the filter needs huge blocks (e.g. 32k for 15k taps), expand the pipeline chunks.
        if (req_block_size > target_block_samples) {
            log_info("FFT filter block size (%zu) exceeds optimal pipeline target (%zu).", req_block_size, target_block_samples);
            log_info("Expanding internal chunk size to accommodate FFT bursts (may reduce CPU cache efficiency).");
            target_block_samples = req_block_size;
        }
    }

    size_t calculated_input_samples = 0;

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
    }
    else {
        // --- CASE B: DOWNSAMPLING / PASSTHROUGH ---
        // The Input is pinned to the Target.
        calculated_input_samples = target_block_samples;
    }

    // --- Calculate Elastic Maximum Buffer Size ---
    // The filter object processes in blocks. If a remainder exists from a previous chunk,
    // the output of the filter can momentarily exceed the input size by up to the FFT block size.
    size_t max_pre_resample_samples = calculated_input_samples;
    if (config->dsp.filter.count > 0 && !config->dsp.filter.apply_post_resample) {
        max_pre_resample_samples += req_block_size;
    }

    // Determine the absolute maximum number of samples that could exist post-resampling
    size_t max_post_resample_samples = (size_t)ceil((double)(max_pre_resample_samples + 32) * resample_ratio) + 64;

    if (config->dsp.filter.count > 0 && config->dsp.filter.apply_post_resample) {
        max_post_resample_samples += req_block_size;
    }

    // The buffer must be large enough to hold the maximum size at ANY stage of the pipeline
    size_t sample_allocation_count = (max_pre_resample_samples > max_post_resample_samples) ? max_pre_resample_samples : max_post_resample_samples;
    sample_allocation_count += PIPELINE_BUFFER_PADDING_SAMPLES;

    // Store the results for runtime usage
    app->pipeline.read_chunk_size = calculated_input_samples;
    app->pipeline.alloc_size_samples = sample_allocation_count;

    // 3. Absolute Chunk Limit Safety Check
    if (app->pipeline.alloc_size_samples > PIPELINE_MAX_CHUNK_SAMPLES) {
        log_error("Calculated pipeline chunk size (%zu samples) exceeds safety limit (%d).",
                  app->pipeline.alloc_size_samples, PIPELINE_MAX_CHUNK_SAMPLES);
        log_error("Try reducing the output sample rate or manually lowering --filter-taps.");
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
        log_fatal("Internal Error: Input sample rate is invalid (%.15g Hz). Cannot calculate buffer depth.", input_rate);
        log_error("Please check the input source configuration.");
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

    log_info("Pipeline Sizing: Read=%zu samples, Alloc=%zu samples, Depth=%zu chunks (%.2f sec buffer at %.15g Hz)",
              app->pipeline.read_chunk_size,
              app->pipeline.alloc_size_samples,
              app->pipeline.num_chunks,
              app->pipeline.num_chunks * seconds_per_chunk,
              input_rate);

    // --- Monolithic Tray Allocation (Contiguous Metadata + Data) ---
    size_t raw_stride     = ALIGN_UP(app->pipeline.alloc_size_samples * app->module.input_bytes_per_iq_sample, MEM_ARENA_ALIGNMENT);
    size_t complex_stride = ALIGN_UP(app->pipeline.alloc_size_samples * sizeof(ComplexFloat), MEM_ARENA_ALIGNMENT);
    app->module.output_bytes_per_iq_sample = get_bytes_per_iq_sample(app->dsp.pipeline_sample_format);
    size_t final_stride   = ALIGN_UP(app->pipeline.alloc_size_samples * app->module.output_bytes_per_iq_sample, MEM_ARENA_ALIGNMENT);

    size_t struct_stride   = ALIGN_UP(sizeof(SampleChunk), MEM_ARENA_ALIGNMENT);
    size_t total_tray_size = struct_stride + raw_stride + (complex_stride * 2) + final_stride;

    // Allocate the big data block
    app->pipeline.chunk_data_pool = aligned_alloc(MEM_ARENA_ALIGNMENT, app->pipeline.num_chunks * total_tray_size);
    if (!app->pipeline.chunk_data_pool) {
        log_fatal("Error: Failed to allocate pipeline chunk data pool.");
        return false;
    }

    // Allocate the Catalog (The array of pointers)
    app->pipeline.sample_chunk_pool = (SampleChunk**)mem_arena_alloc(&app->pipeline.setup_arena, app->pipeline.num_chunks * sizeof(SampleChunk*), true);

    for (size_t i = 0; i < app->pipeline.num_chunks; ++i) {
        // Calculate the base address for this specific tray
        uint8_t* tray_base = (uint8_t*)app->pipeline.chunk_data_pool + (i * total_tray_size);

        // The Catalog entry points to the struct at the front of the tray
        app->pipeline.sample_chunk_pool[i] = (SampleChunk*)tray_base;
        SampleChunk* item = app->pipeline.sample_chunk_pool[i];

        // The buffers follow immediately after the metadata struct
        uint8_t* data_ptr = tray_base + struct_stride;

        item->raw_input_data         = data_ptr;
        data_ptr += raw_stride;
        item->pre_resample_buffer = (ComplexFloat*)data_ptr;
        data_ptr += complex_stride;
        item->post_resample_buffer = (ComplexFloat*)data_ptr;
        data_ptr += complex_stride;
        item->final_output_data      = (unsigned char*)data_ptr;

        // Set capacities and metadata
        item->raw_input_capacity_bytes = raw_stride;
        item->complex_buffer_capacity_samples = app->pipeline.alloc_size_samples;
        item->final_output_capacity_bytes = final_stride;
        item->input_bytes_per_iq_sample = app->module.input_bytes_per_iq_sample;
    }

    // -------------------------------------------------------------------------
    // 5. Calculate Dynamic Ring Buffer Sizes
    // -------------------------------------------------------------------------

    // Calculate input buffer size (for buffered mode)
    if (app->pipeline_mode != PIPELINE_MODE_SYNCHRONOUS_PULL) {
        size_t input_buffer_bytes = (size_t)(
            input_rate *
            INPUT_BUFFER_DURATION_SEC *
            app->module.input_bytes_per_iq_sample
        );

        if (input_buffer_bytes < INPUT_BUFFER_MIN_BYTES)
            input_buffer_bytes = INPUT_BUFFER_MIN_BYTES;
        if (input_buffer_bytes > INPUT_BUFFER_MAX_BYTES)
            input_buffer_bytes = INPUT_BUFFER_MAX_BYTES;

        app->pipeline.input_buffer_size = input_buffer_bytes;

        log_info("Input Buffer: Allocating %zu bytes (%.2f sec capacity) at %.15g Hz.",
                 input_buffer_bytes,
                 INPUT_BUFFER_DURATION_SEC,
                 input_rate);
    }

    return true;
}

bool pipeline_setup_buffers(PipelineContext* context) {
    AppConfig* config = context->config;
    AppContext* app = context->app;

    // --- Step 0: Calculate Ratios & Allocate Memory Pools ---
    if (!calculate_and_validate_resample_ratio(config, app, &app->dsp.resample_ratio)) return false;
    if (!allocate_processing_buffers(config, app, app->dsp.resample_ratio)) return false;

    return true;
}

bool pipeline_run(PipelineContext* context) {
    AppConfig* config = context->config;
    AppContext* app = context->app;
    bool success = false;

    // --- Step 1: Create all internal DSP components ---
    if (!_create_dsp_components(config, app, app->dsp.resample_ratio)) {
        log_fatal("Failed to create DSP components.");
        _destroy_dsp_components(app); // Attempt cleanup
        return false;
    }

    // --- Step 2: Verify memory pools (Allocated during initialization) ---
    if (!app->pipeline.chunk_data_pool) {
        log_fatal("Pipeline memory pool not allocated. Initialization order error.");
        _destroy_dsp_components(app);
        return false;
    }

    // --- Step 3: Create and wire all communication channels ---
    if (!_init_queues_and_buffers(config, app)) {
        log_fatal("Failed to initialize pipeline queues and buffers.");
        _destroy_queues_and_buffers(app);
        _destroy_dsp_components(app);
        return false;
    }

    // --- Step 4: Initialize the generic thread manager ---
    ThreadManager manager;
    thread_manager_init(&manager, context);

    // --- Step 5: Spawn threads based on configuration (Direct Command Model) ---
    log_debug("Spawning pipeline threads...");
    bool threads_ok = true;
    if (app->pipeline_mode != PIPELINE_MODE_SYNCHRONOUS_PULL) {
        if (!thread_manager_spawn_thread(&manager, "Source", pipeline_thread_source)) threads_ok = false;
    }
    if (threads_ok && !thread_manager_spawn_thread(&manager, "Reader", pipeline_thread_reader)) threads_ok = false;
    if (threads_ok && !config->dsp.raw_passthrough) {
        if (!thread_manager_spawn_thread(&manager, "Pre-Processor", pipeline_thread_pre_processor)) threads_ok = false;
        if (threads_ok && !app->dsp.bypass_resampler) {
            if (!thread_manager_spawn_thread(&manager, "Resampler", pipeline_thread_resampler)) threads_ok = false;
        }
        if (threads_ok && !thread_manager_spawn_thread(&manager, "Post-Processor", pipeline_thread_post_processor)) threads_ok = false;
    }
    if (threads_ok && !thread_manager_spawn_thread(&manager, "Writer", pipeline_thread_writer)) threads_ok = false;
    if (threads_ok && config->dsp.iq_correction.enable) {
        if (!thread_manager_spawn_thread(&manager, "I/Q Optimizer", pipeline_thread_iq_estimator)) threads_ok = false;
    }
    if (threads_ok && module_is_live_source(config->input.type_name, &app->pipeline.setup_arena)) {
        if (!thread_manager_spawn_thread(&manager, "Source Watchdog", pipeline_thread_watchdog)) threads_ok = false;
    }

    if (!threads_ok) {
        log_fatal("Failed to spawn one or more pipeline threads. Initiating shutdown.");
        request_shutdown(); // Signal any successfully started threads to stop
    }

    // --- Step 6: Wait for all spawned threads to complete ---
    thread_manager_join_all(&manager);
    log_debug("All pipeline threads have completed.");
    success = !atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed);

    // --- Step 7: Clean up all pipeline-specific app ---
    _destroy_queues_and_buffers(app);
    _destroy_dsp_components(app);

    return success;
}

// --- Private Helper Function Implementations ---

static bool _create_dsp_components(AppConfig* config, AppContext* app, float resample_ratio) {
    if (!dc_block_create(config, app)) return false;
    if (!iq_correction_init(config, app, &app->pipeline.setup_arena)) return false;
    if (!frequency_shift_create(config, app)) return false;
    app->dsp.resampler = resampler_create(config, app, resample_ratio);
    if (!app->dsp.resampler && !app->dsp.bypass_resampler) return false;
    if (!filter_create(config, app, &app->pipeline.setup_arena)) return false;
    if (!agc_create(config, app)) return false;

    // Initialize the main shutdown event
    app->pipeline.shutdown_event = wait_event_create(&app->pipeline.setup_arena);
    if (!app->pipeline.shutdown_event) {
        log_fatal("Failed to create shutdown event.");
        return false;
    }
    return true;
}

static void _destroy_dsp_components(AppContext* app) {
    agc_destroy(app);
    filter_destroy(app);
    resampler_destroy(app->dsp.resampler);
    app->dsp.resampler = NULL;
    frequency_shift_destroy_ncos(app);
    iq_correction_destroy(app);
    dc_block_destroy(app);
}

static bool _init_queues_and_buffers(AppConfig* config, AppContext* app) {
    MemoryArena* arena = &app->pipeline.setup_arena;
    Queue* last_output_queue = NULL;

    // CRITICAL: Use the dynamically calculated chunk count for queue sizing
    size_t queue_capacity = app->pipeline.num_chunks;

    app->pipeline.reader_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!app->pipeline.reader_output_queue || !queue_init(app->pipeline.reader_output_queue, queue_capacity, arena)) return false;
    last_output_queue = app->pipeline.reader_output_queue;

    if (!config->dsp.raw_passthrough) {
        app->pipeline.pre_processor_input_queue = last_output_queue;
        app->pipeline.pre_processor_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!app->pipeline.pre_processor_output_queue || !queue_init(app->pipeline.pre_processor_output_queue, queue_capacity, arena)) return false;
        last_output_queue = app->pipeline.pre_processor_output_queue;
    }

    if (!config->dsp.raw_passthrough && !app->dsp.bypass_resampler) {
        app->pipeline.resampler_input_queue = last_output_queue;
        app->pipeline.resampler_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!app->pipeline.resampler_output_queue || !queue_init(app->pipeline.resampler_output_queue, queue_capacity, arena)) return false;
        last_output_queue = app->pipeline.resampler_output_queue;
    }

    if (!config->dsp.raw_passthrough) {
        app->pipeline.post_processor_input_queue = last_output_queue;
        app->pipeline.post_processor_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!app->pipeline.post_processor_output_queue || !queue_init(app->pipeline.post_processor_output_queue, queue_capacity, arena)) return false;
        last_output_queue = app->pipeline.post_processor_output_queue;
    }

    app->pipeline.writer_input_queue = last_output_queue;

    app->pipeline.free_sample_chunk_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!queue_init(app->pipeline.free_sample_chunk_queue, queue_capacity, arena)) return false;

    if (config->dsp.iq_correction.enable) {
        app->pipeline.iq_estimation_data_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!queue_init(app->pipeline.iq_estimation_data_queue, queue_capacity, arena)) return false;

        app->pipeline.iq_estimation_free_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!queue_init(app->pipeline.iq_estimation_free_queue, queue_capacity, arena)) return false;

        for (int i = 0; i < 16; i++) {
            void* buffer = mem_arena_alloc(arena, 4096 * sizeof(ComplexFloat), false); // 4096 is IQ_CORRECTION_FFT_SIZE
            queue_enqueue(app->pipeline.iq_estimation_free_queue, buffer);
        }
    }

    // Populate the free queue with the pre-allocated chunks
    for (size_t i = 0; i < app->pipeline.num_chunks; ++i) {
        if (!queue_enqueue(app->pipeline.free_sample_chunk_queue, app->pipeline.sample_chunk_pool[i])) {
            log_fatal("Failed to initially populate free item queue.");
            return false;
        }
    }

    if (app->pipeline_mode != PIPELINE_MODE_SYNCHRONOUS_PULL) {
        app->pipeline.source_input_buffer = ring_buffer_create(app->pipeline.input_buffer_size, arena);
        if (!app->pipeline.source_input_buffer) return false;
    }
    return true;
}

static void _destroy_queues_and_buffers(AppContext* app) {
    if (!app) return;

    if (app->pipeline.shutdown_event) {
        wait_event_destroy(app->pipeline.shutdown_event);
    }

    if (app->pipeline.source_input_buffer) ring_buffer_destroy(app->pipeline.source_input_buffer);

    if(app->pipeline.free_sample_chunk_queue) queue_destroy(app->pipeline.free_sample_chunk_queue);
    if(app->pipeline.reader_output_queue) queue_destroy(app->pipeline.reader_output_queue);
    if(app->pipeline.pre_processor_output_queue) queue_destroy(app->pipeline.pre_processor_output_queue);
    if(app->pipeline.resampler_output_queue) queue_destroy(app->pipeline.resampler_output_queue);
    if(app->pipeline.post_processor_output_queue) queue_destroy(app->pipeline.post_processor_output_queue);
    if(app->pipeline.iq_estimation_data_queue) queue_destroy(app->pipeline.iq_estimation_data_queue);
    if(app->pipeline.iq_estimation_free_queue) queue_destroy(app->pipeline.iq_estimation_free_queue);
}
