/**
 * @file pipeline.c
 * @brief Implements the creation, execution, and destruction of the application's DSP pipeline.
 *
 * This module is the central orchestrator for the application's concurrent processing.
 * It contains the master `pipeline_run` function, as well as the private implementations
 * for all the pipeline's concurrent stages and utility threads.
 */

#include "pipeline.h"
#include "pipeline_threads.h"
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


// --- Pipeline Thread Function Implementations (Private to this module) ---

// Universal Ingest Callback
static bool pipeline_queue_samples(void* ctx, const void* data, size_t num_samples, SampleFormat format) {
    AppContext* app = (AppContext*)ctx;
    sdr_input_update_heartbeat(app);
    if (is_shutdown_requested() || atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed)) return false;

    if (!packet_serializer_write_block(app->pipeline.sdr_input_buffer, num_samples, data, format)) {
        log_warn("Pipeline input overrun! Dropped %zu samples.", num_samples);
        return false;
    }
    return true;
}

void* pipeline_thread_sdr_capture(void* arg) {
    platform_set_thread_priority(PRIORITY_REALTIME, "SDR Capture");

    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;
    ModuleContext ctx = { .config = args->config, .app = app };

    app->module.input_api->start_stream(&ctx, pipeline_queue_samples, app);

    if (app->pipeline.sdr_input_buffer) {
        ring_buffer_signal_end_of_stream(app->pipeline.sdr_input_buffer);
    }

    log_debug("SDR capture thread is exiting.");
    return NULL;
}

void* pipeline_thread_reader(void* arg) {
    platform_set_thread_priority(PRIORITY_NORMAL, "Reader");

    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;
    AppConfig* config = args->config;

    switch (app->pipeline_mode) {
        case PIPELINE_MODE_BUFFERED_INPUT: {
            log_debug("Reader thread starting.");

            // --- STATEFUL SIPPING LOGIC ---
            // Initialize the serializer state for this thread.
            SerializerState state;
            memset(&state, 0, sizeof(state));

            while (!is_shutdown_requested() && !atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed)) {
                SampleChunk* item = (SampleChunk*)queue_dequeue(app->pipeline.free_sample_chunk_queue);
                if (!item) break;

                bool is_reset = false;

                // Call the serializer with the state and the calculated elastic request size
                int64_t frames_read = packet_serializer_read_packet(
                    app->pipeline.sdr_input_buffer,
                    item,
                    &state,
                    &is_reset,
                    app->pipeline.read_chunk_size
                );

                if (frames_read < 0) {
                    handle_fatal_thread_error("Reader: Fatal error parsing SDR buffer stream.", app);
                    queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
                    break;
                }

                if (frames_read == 0 && !is_reset) {
                    item->is_last_chunk = true;
                    item->frames_read = 0;
                    queue_enqueue(app->pipeline.reader_output_queue, item);
                    break;
                }

                item->frames_read = frames_read;
                item->frames_to_write = (unsigned int)frames_read;
                item->stream_discontinuity_event = is_reset;
                item->is_last_chunk = false;

                // In Buffered mode, the Pre-Processor is skipped during passthrough.
                // We must manually copy data to the final buffer so the Writer can find it.
                if (config->dsp.raw_passthrough && item->frames_read > 0) {
                    size_t bytes = item->frames_read * item->input_bytes_per_sample_pair;
                    memcpy(item->final_output_data, item->raw_input_data, bytes);
                }

                if (item->frames_read > 0) {
                    atomic_fetch_add_explicit(&app->stats.total_frames_read, item->frames_read, memory_order_relaxed);
                }

                if (!queue_enqueue(app->pipeline.reader_output_queue, item)) {
                    // The pipeline is shutting down, so we can't send data forward.
                    // We drop the data, but we MUST return the memory to the pool.
                    // We use forced enqueue to guarantee the pool accepts it.
                    queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
                    break;
                }
            }
            break;
        }

        case PIPELINE_MODE_FILE_PROCESSING: {
            ModuleContext ctx = { .config = config, .app = app };
            InputModuleInterface* in_api = app->module.input_api;
            
            if (!in_api->read_chunk) {
                handle_fatal_thread_error("Reader: File input module missing read_chunk.", app);
            } else {
                while (!is_shutdown_requested() && !atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed)) {
                    SampleChunk* item = (SampleChunk*)queue_dequeue(app->pipeline.free_sample_chunk_queue);
                    if (!item) break;

                    size_t bytes_requested = app->pipeline.read_chunk_size * app->module.input_bytes_per_sample_pair;
                    size_t capacity_bytes = item->raw_input_capacity_bytes;
                    if (bytes_requested > capacity_bytes) bytes_requested = capacity_bytes;

                    void* target_buffer = config->dsp.raw_passthrough ? item->final_output_data : item->raw_input_data;
                    size_t bytes_read = in_api->read_chunk(&ctx, target_buffer, bytes_requested);

                    item->frames_read = bytes_read / app->module.input_bytes_per_sample_pair;
                    item->frames_to_write = (unsigned int)item->frames_read;
                    item->packet_sample_format = app->module.input_format;
                    item->input_bytes_per_sample_pair = app->module.input_bytes_per_sample_pair;
                    item->stream_discontinuity_event = false;
                    item->is_last_chunk = (bytes_read < bytes_requested); // EOF reached

                    if (item->frames_read > 0) {
                        atomic_fetch_add_explicit(&app->stats.total_frames_read, item->frames_read, memory_order_relaxed);
                    }

                    if (!queue_enqueue(app->pipeline.reader_output_queue, item)) {
                        queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
                        break;
                    }
                    if (item->is_last_chunk) break;
                }
            }
            break;
        }
    }

    if (!is_shutdown_requested()) {
        log_debug("Reader thread finished naturally. End of stream reached.");
        atomic_store_explicit(&app->stats.end_of_stream_reached, true, memory_order_release);
    } else {
        SampleChunk *last_item = (SampleChunk*)queue_try_dequeue(app->pipeline.free_sample_chunk_queue);
        if (last_item) {
             last_item->is_last_chunk = true;
             last_item->frames_read = 0;
             queue_enqueue(app->pipeline.reader_output_queue, last_item);
        }
    }

    log_debug("Reader thread is exiting.");
    return NULL;
}

void* pipeline_thread_writer(void* arg) {
    platform_set_thread_priority(PRIORITY_HIGHEST, "Writer");

    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;
    OutputModuleInterface* out_api = app->module.output_api;
    ModuleContext ctx = { .config = args->config, .app = app };

    if (!out_api || !out_api->write_chunk) {
        log_fatal("Output module does not implement write_chunk.");
        return NULL;
    }

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(app->pipeline.writer_input_queue);
        if (!item) break; // Shutdown signaled

        // 1. Process payload FIRST if it exists
        // This ensures the partial "tail" of a file is written before we exit.
        if (item->frames_to_write > 0 && !item->stream_discontinuity_event) {
            size_t bytes_to_write = item->frames_to_write * app->module.output_bytes_per_sample_pair;
            size_t written = out_api->write_chunk(&ctx, item->final_output_data, bytes_to_write);

            // Stats & Progress Reporting (Only for file outputs)
            if (args->config->output.path_arg != NULL && app->stats.progress_callback && written > 0) {
                atomic_fetch_add_explicit(&app->stats.total_output_frames, item->frames_to_write, memory_order_relaxed);
                // Correctly track bytes and signal progress
                int64_t total_bytes = atomic_fetch_add_explicit(&app->stats.final_output_size_bytes, (int64_t)written, memory_order_relaxed) + (int64_t)written;

                app->stats.progress_callback(
                    atomic_load_explicit(&app->stats.total_output_frames, memory_order_relaxed),
                    atomic_load_explicit(&app->stats.expected_total_output_frames, memory_order_relaxed),
                    total_bytes,
                    app->stats.progress_callback_udata);
            }
        }

        // 2. Handle Reset Event (SDR Overrun)
        if (item->stream_discontinuity_event) {
            if (out_api->reset) out_api->reset(&ctx);
        }

        // 3. Handle End-of-Stream
        if (item->is_last_chunk) {
            if (out_api->flush) out_api->flush(&ctx);
            queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
            break; // Loop exit happens here, AFTER processing data/flush
        }

        // 4. Return regular chunks to the pool
        if (!queue_enqueue(app->pipeline.free_sample_chunk_queue, item)) break;
    }

    log_debug("Generic Writer thread is exiting.");
    return NULL;
}

void* pipeline_thread_pre_processor(void* arg) {
    platform_set_thread_priority(PRIORITY_HIGH, "Pre-Processor");

    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;
    AppConfig* config = args->config;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(app->pipeline.pre_processor_input_queue)) != NULL) {

        if (item->is_last_chunk) {
            if (app->pipeline.iq_optimization_data_queue) {
                queue_signal_shutdown(app->pipeline.iq_optimization_data_queue);
            }
            queue_enqueue(app->pipeline.pre_processor_output_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            pre_processor_reset(&app->dsp);
            if (!queue_enqueue(app->pipeline.pre_processor_output_queue, item)) {
                break;
            }
            continue;
        }

        pre_processor_apply_chain(&app->dsp, item);

        if (app->dsp.is_passthrough) {
            item->frames_to_write = (unsigned int)item->frames_read;
            }

        if (config->dsp.iq_correction.enable) {
            if (item->frames_read >= IQ_CORRECTION_FFT_SIZE && !item->stream_discontinuity_event) {
                SampleChunk* opt_item = (SampleChunk*)queue_try_dequeue(app->pipeline.free_sample_chunk_queue);
                if (opt_item) {
                    memcpy(opt_item->complex_sample_buffer_a, item->complex_sample_buffer_a, IQ_CORRECTION_FFT_SIZE * sizeof(ComplexFloat));
                    queue_enqueue(app->pipeline.iq_optimization_data_queue, opt_item);
                }
            }
        }

        if (item->frames_read > 0) {
            if (!queue_enqueue(app->pipeline.pre_processor_output_queue, item)) {
                // Downstream rejected us (shutdown). Force return to pool.
                queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
                break;
            }
        } else {
            // Empty/Control chunk, just return it.
            queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
        }
    }

    log_debug("Pre-processor thread is exiting.");
    return NULL;
}

void* pipeline_thread_resampler(void* arg) {
    platform_set_thread_priority(PRIORITY_NORMAL, "Resampler");

    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(app->pipeline.resampler_input_queue)) != NULL) {
        if (item->is_last_chunk) {
            queue_enqueue(app->pipeline.resampler_output_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            resampler_reset(app->dsp.resampler);
            if (!queue_enqueue(app->pipeline.resampler_output_queue, item)) {
                break;
            }
            continue;
        }

        // Set up the state pointers for this stage
        if (item->current_input_buffer == item->complex_sample_buffer_a) {
            item->current_output_buffer = item->complex_sample_buffer_b;
        } else {
            item->current_output_buffer = item->complex_sample_buffer_a;
        }

        unsigned int output_frames_this_chunk = 0;
        if (app->dsp.is_passthrough) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            // In passthrough, we must copy the data to the output buffer
            memcpy(item->current_output_buffer, item->current_input_buffer, output_frames_this_chunk * sizeof(ComplexFloat));
        } else {
            // Estimate maximum output length mathematically prior to execution
            unsigned int estimated_out = (unsigned int)((item->frames_read + 32) * app->dsp.resample_ratio) + 64;
            if (estimated_out > item->complex_buffer_capacity_samples) {
                handle_fatal_thread_error("Resampler input chunk is too large for ping-pong buffers!", app);
                break;
            }
            resampler_execute(app->dsp.resampler,
                              item->current_input_buffer,
                              (unsigned int)item->frames_read,
                              item->current_output_buffer,
                              &output_frames_this_chunk);
        }
        item->frames_to_write = output_frames_this_chunk;

        // Output becomes input for next stage
        item->current_input_buffer = item->current_output_buffer;

        if (!queue_enqueue(app->pipeline.resampler_output_queue, item)) {
            // Downstream rejected us. Force return to pool.
            queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
            break;
        }
    }
    log_debug("Resampler thread is exiting.");
    return NULL;
}

void* pipeline_thread_post_processor(void* arg) {
    platform_set_thread_priority(PRIORITY_HIGH, "Post-Processor");

    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(app->pipeline.post_processor_input_queue)) != NULL) {

        if (item->is_last_chunk) {
            // Unified: Always pass the chunk to the writer thread's queue.
            queue_enqueue(app->pipeline.writer_input_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            post_processor_reset(&app->dsp);
            if (!queue_enqueue(app->pipeline.post_processor_output_queue, item)) {
                break;
            }
            continue;
        }

        post_processor_apply_chain(&app->dsp, item);

        if (item->frames_to_write > 0) {
            // Unified: Backpressure is handled by the writer thread now.
            if (!queue_enqueue(app->pipeline.writer_input_queue, item)) {
                queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
                break;
            }
        } else {
            // Empty frame, force return to pool
            queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
        }
    }

    log_debug("Post-processor thread is exiting.");
    return NULL;
}
