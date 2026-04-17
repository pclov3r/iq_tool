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
#include "utils.h"
#include "platform.h" // Added for thread priority abstraction
#include "signal_handler.h"
#include "log.h"
#include "module_registry.h"
#include "pre_processor.h"
#include "post_processor.h"
#include "dc_block.h"
#include "iq_correction.h"
#include "freq_shift.h"
#include "resampler.h"
#include "filter.h"
#include "agc.h" // Added for Output AGC
#include "sample_convert.h"
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

    // --- Step 2: Allocate all memory pools (Now handled in setup.c via allocate_processing_buffers) ---
    // Note: The memory pool allocation was moved to setup.c to happen earlier in the lifecycle.
    // We check if it was successful here.
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
    if (app->pipeline_mode != PIPELINE_MODE_FILE_PROCESSING) {
        if (!thread_manager_spawn_thread(&manager, "SDR Capture", pipeline_thread_sdr_capture)) threads_ok = false;
    }
    if (threads_ok && !thread_manager_spawn_thread(&manager, "Reader", pipeline_thread_reader)) threads_ok = false;
    if (threads_ok && !config->dsp.raw_passthrough) {
        if (!thread_manager_spawn_thread(&manager, "Pre-Processor", pipeline_thread_pre_processor)) threads_ok = false;
        if (threads_ok && !app->dsp.is_passthrough) {
            if (!thread_manager_spawn_thread(&manager, "Resampler", pipeline_thread_resampler)) threads_ok = false;
        }
        if (threads_ok && !thread_manager_spawn_thread(&manager, "Post-Processor", pipeline_thread_post_processor)) threads_ok = false;
    }
    if (threads_ok && !thread_manager_spawn_thread(&manager, "Writer", pipeline_thread_writer)) threads_ok = false;
    if (threads_ok && config->dsp.iq_correction.enable) {
        if (!thread_manager_spawn_thread(&manager, "I/Q Optimizer", pipeline_thread_iq_optimizer)) threads_ok = false;
    }
    if (threads_ok && module_is_sdr(config->input.type_name, &app->pipeline.setup_arena)) {
        if (!thread_manager_spawn_thread(&manager, "SDR Watchdog", pipeline_thread_watchdog)) threads_ok = false;
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
    if (!freq_shift_create(config, app)) return false;
    app->dsp.resampler = resampler_create(config, app, resample_ratio);
    if (!app->dsp.resampler && !app->dsp.is_passthrough) return false;
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
    freq_shift_destroy_ncos(app);
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

    if (!config->dsp.raw_passthrough && !app->dsp.is_passthrough) {
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
        app->pipeline.iq_optimization_data_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!queue_init(app->pipeline.iq_optimization_data_queue, queue_capacity, arena)) return false;
    }

    // Populate the free queue with the pre-allocated chunks
    for (size_t i = 0; i < app->pipeline.num_chunks; ++i) {
        if (!queue_enqueue(app->pipeline.free_sample_chunk_queue, &app->pipeline.sample_chunk_pool[i])) {
            log_fatal("Failed to initially populate free item queue.");
            return false;
        }
    }

    if (app->pipeline_mode != PIPELINE_MODE_FILE_PROCESSING) {
        app->pipeline.sdr_input_buffer = ring_buffer_create(app->pipeline.input_buffer_size);
        if (!app->pipeline.sdr_input_buffer) return false;
    }



    return true;
}

static void _destroy_queues_and_buffers(AppContext* app) {
    if (!app) return;

    if (app->pipeline.shutdown_event) {
        wait_event_destroy(app->pipeline.shutdown_event);
    }

    if (app->pipeline.sdr_input_buffer) ring_buffer_destroy(app->pipeline.sdr_input_buffer);

    if(app->pipeline.free_sample_chunk_queue) queue_destroy(app->pipeline.free_sample_chunk_queue);
    if(app->pipeline.reader_output_queue) queue_destroy(app->pipeline.reader_output_queue);
    if(app->pipeline.pre_processor_output_queue) queue_destroy(app->pipeline.pre_processor_output_queue);
    if(app->pipeline.resampler_output_queue) queue_destroy(app->pipeline.resampler_output_queue);
    if(app->pipeline.post_processor_output_queue) queue_destroy(app->pipeline.post_processor_output_queue);
    if(app->pipeline.iq_optimization_data_queue) queue_destroy(app->pipeline.iq_optimization_data_queue);
}
