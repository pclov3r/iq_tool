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
#include "constants.h"
#include "app_context.h"
#include "utils.h"
#include "platform.h" // Added for thread priority abstraction
#include "signal_handler.h"
#include "log.h"
#include "module_manager.h"
#include "pre_processor.h"
#include "post_processor.h"
#include "dc_block.h"
#include "iq_correct.h"
#include "frequency_shift.h"
#include "resampler.h"
#include "filter.h"
#include "agc.h" // Added for Output AGC
#include "sample_convert.h"
#include "queue.h"
#include "ring_buffer.h"
#include "sdr_packet_serializer.h"
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
static bool _init_queues_and_buffers(AppConfig* config, AppResources* resources);
static void _destroy_queues_and_buffers(AppResources* resources);
static bool _create_dsp_components(AppConfig* config, AppResources* resources, float resample_ratio);
static void _destroy_dsp_components(AppResources* resources);


/**
 * @brief Creates, runs, and waits for the entire processing pipeline to complete.
 *
 * This is the main high-level function that encapsulates the entire pipeline lifecycle.
 * It handles the creation of all DSP objects and queues, spawns all necessary threads
 * using the thread manager, waits for them to finish, and then cleans up all
 * pipeline-specific resources.
 *
 * @param context A pointer to the PipelineContext, containing the application config and resources.
 * @return true if the pipeline ran and shut down cleanly, false if there was a setup or execution error.
 */
bool pipeline_run(PipelineContext* context) {
    AppConfig* config = context->config;
    AppResources* resources = context->resources;
    bool success = false;

    // --- Step 1: Create all internal DSP components ---
    if (!_create_dsp_components(config, resources, resources->resample_ratio)) {
        log_fatal("Failed to create DSP components.");
        _destroy_dsp_components(resources); // Attempt cleanup
        return false;
    }

    // --- Step 2: Allocate all memory pools (Now handled in setup.c via allocate_processing_buffers) ---
    // Note: The memory pool allocation was moved to setup.c to happen earlier in the lifecycle.
    // We check if it was successful here.
    if (!resources->pipeline_chunk_data_pool) {
        log_fatal("Pipeline memory pool not allocated. Initialization order error.");
        _destroy_dsp_components(resources);
        return false;
    }

    // --- Step 3: Create and wire all communication channels ---
    if (!_init_queues_and_buffers(config, resources)) {
        log_fatal("Failed to initialize pipeline queues and buffers.");
        _destroy_queues_and_buffers(resources);
        _destroy_dsp_components(resources);
        return false;
    }

    // --- Step 4: Initialize the generic thread manager ---
    ThreadManager manager;
    thread_manager_init(&manager, context);

    // --- Step 5: Spawn threads based on configuration (Direct Command Model) ---
    log_debug("Spawning pipeline threads...");
    bool threads_ok = true;
    if (resources->pipeline_mode == PIPELINE_MODE_BUFFERED_SDR) {
        if (!thread_manager_spawn_thread(&manager, "SDR Capture", sdr_capture_thread_func)) threads_ok = false;
    }
    if (threads_ok && !thread_manager_spawn_thread(&manager, "Reader", reader_thread_func)) threads_ok = false;
    if (threads_ok && !config->dsp.raw_passthrough) {
        if (!thread_manager_spawn_thread(&manager, "Pre-Processor", pre_processor_thread_func)) threads_ok = false;
        if (threads_ok && !resources->is_passthrough) {
            if (!thread_manager_spawn_thread(&manager, "Resampler", resampler_thread_func)) threads_ok = false;
        }
        if (threads_ok && !thread_manager_spawn_thread(&manager, "Post-Processor", post_processor_thread_func)) threads_ok = false;
    }
    if (threads_ok && !thread_manager_spawn_thread(&manager, "Writer", writer_thread_func)) threads_ok = false;
    if (threads_ok && config->dsp.iq_correction.enable) {
        if (!thread_manager_spawn_thread(&manager, "I/Q Optimizer", iq_optimization_thread_func)) threads_ok = false;
    }
    if (threads_ok && module_manager_is_sdr_module(config->input.type_name, &resources->setup_arena)) {
        if (!thread_manager_spawn_thread(&manager, "SDR Watchdog", watchdog_thread_func)) threads_ok = false;
    }

    if (!threads_ok) {
        log_fatal("Failed to spawn one or more pipeline threads. Initiating shutdown.");
        request_shutdown(); // Signal any successfully started threads to stop
    }

    // --- Step 6: Wait for all spawned threads to complete ---
    thread_manager_join_all(&manager);
    log_debug("All pipeline threads have completed.");
    success = !resources->error_occurred;

    // --- Step 7: Clean up all pipeline-specific resources ---
    _destroy_queues_and_buffers(resources);
    _destroy_dsp_components(resources);

    return success;
}


// --- Private Helper Function Implementations ---

static bool _create_dsp_components(AppConfig* config, AppResources* resources, float resample_ratio) {
    if (!dc_block_create(config, resources)) return false;
    if (!iq_correct_init(config, resources, &resources->setup_arena)) return false;
    if (!freq_shift_create(config, resources)) return false;
    resources->resampler = create_resampler(config, resources, resample_ratio);
    if (!resources->resampler && !resources->is_passthrough) return false;
    if (!filter_create(config, resources, &resources->setup_arena)) return false;
    if (!agc_create(config, resources)) return false;
    return true;
}

static void _destroy_dsp_components(AppResources* resources) {
    agc_destroy(resources);
    filter_destroy(resources);
    destroy_resampler(resources->resampler);
    resources->resampler = NULL;
    freq_shift_destroy_ncos(resources);
    iq_correct_destroy(resources);
    dc_block_destroy(resources);
}

static bool _init_queues_and_buffers(AppConfig* config, AppResources* resources) {
    MemoryArena* arena = &resources->setup_arena;
    Queue* last_output_queue = NULL;

    // CRITICAL: Use the dynamically calculated chunk count for queue sizing
    size_t queue_capacity = resources->pipeline_num_chunks;

    resources->reader_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!resources->reader_output_queue || !queue_init(resources->reader_output_queue, queue_capacity, arena)) return false;
    last_output_queue = resources->reader_output_queue;

    if (!config->dsp.raw_passthrough) {
        resources->pre_processor_input_queue = last_output_queue;
        resources->pre_processor_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!resources->pre_processor_output_queue || !queue_init(resources->pre_processor_output_queue, queue_capacity, arena)) return false;
        last_output_queue = resources->pre_processor_output_queue;
    }

    if (!config->dsp.raw_passthrough && !resources->is_passthrough) {
        resources->resampler_input_queue = last_output_queue;
        resources->resampler_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!resources->resampler_output_queue || !queue_init(resources->resampler_output_queue, queue_capacity, arena)) return false;
        last_output_queue = resources->resampler_output_queue;
    }

    if (!config->dsp.raw_passthrough) {
        resources->post_processor_input_queue = last_output_queue;
        resources->post_processor_output_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!resources->post_processor_output_queue || !queue_init(resources->post_processor_output_queue, queue_capacity, arena)) return false;
        last_output_queue = resources->post_processor_output_queue;
    }

    resources->writer_input_queue = last_output_queue;

    resources->free_sample_chunk_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!queue_init(resources->free_sample_chunk_queue, queue_capacity, arena)) return false;

    if (config->dsp.iq_correction.enable) {
        resources->iq_optimization_data_queue = (Queue*)mem_arena_alloc(arena, sizeof(Queue), true);
        if (!queue_init(resources->iq_optimization_data_queue, queue_capacity, arena)) return false;
    }

    // Populate the free queue with the pre-allocated chunks
    for (size_t i = 0; i < resources->pipeline_num_chunks; ++i) {
        if (!queue_enqueue(resources->free_sample_chunk_queue, &resources->sample_chunk_pool[i])) {
            log_fatal("Failed to initially populate free item queue.");
            return false;
        }
    }

    if (resources->pipeline_mode == PIPELINE_MODE_BUFFERED_SDR) {
        resources->sdr_input_buffer = ring_buffer_create(IO_SDR_INPUT_BUFFER_BYTES);
        if (!resources->sdr_input_buffer) return false;
    }
    
    if (resources->pacing_is_required) {
        resources->writer_input_buffer = ring_buffer_create(IO_OUTPUT_WRITER_BUFFER_BYTES);
        if (!resources->writer_input_buffer) return false;
    }

    return true;
}

static void _destroy_queues_and_buffers(AppResources* resources) {
    if (!resources) return;

    if (resources->sdr_input_buffer) ring_buffer_destroy(resources->sdr_input_buffer);
    if (resources->writer_input_buffer) ring_buffer_destroy(resources->writer_input_buffer);

    if(resources->free_sample_chunk_queue) queue_destroy(resources->free_sample_chunk_queue);
    if(resources->reader_output_queue) queue_destroy(resources->reader_output_queue);
    if(resources->pre_processor_output_queue) queue_destroy(resources->pre_processor_output_queue);
    if(resources->resampler_output_queue) queue_destroy(resources->resampler_output_queue);
    if(resources->post_processor_output_queue) queue_destroy(resources->post_processor_output_queue);
    if(resources->iq_optimization_data_queue) queue_destroy(resources->iq_optimization_data_queue);
}


// --- Pipeline Thread Function Implementations (Private to this module) ---

void* sdr_capture_thread_func(void* arg) {
    platform_set_thread_priority(PRIORITY_REALTIME, "SDR Capture");

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    ModuleContext ctx = { .config = args->config, .resources = resources };

    resources->selected_input_module_api->start_stream(&ctx);

    if (resources->sdr_input_buffer) {
        ring_buffer_signal_end_of_stream(resources->sdr_input_buffer);
    }

    log_debug("SDR capture thread is exiting.");
    return NULL;
}

void* reader_thread_func(void* arg) {
    platform_set_thread_priority(PRIORITY_NORMAL, "Reader");

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    switch (resources->pipeline_mode) {
        case PIPELINE_MODE_BUFFERED_SDR: {
            log_debug("Reader thread starting in buffered SDR mode.");

            // --- STATEFUL SIPPING LOGIC ---
            // Initialize the serializer state for this thread.
            SerializerState state;
            memset(&state, 0, sizeof(state));

            while (!is_shutdown_requested() && !resources->error_occurred) {
                SampleChunk* item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (!item) break;

                bool is_reset = false;

                // Call the serializer with the state and the calculated elastic request size
                int64_t frames_read = sdr_packet_serializer_read_packet(
                    resources->sdr_input_buffer,
                    item,
                    &state,
                    &is_reset,
                    resources->pipeline_read_chunk_size
                );

                if (frames_read < 0) {
                    handle_fatal_thread_error("Reader: Fatal error parsing SDR buffer stream.", resources);
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                    break;
                }

                if (frames_read == 0 && !is_reset) {
                    item->is_last_chunk = true;
                    item->frames_read = 0;
                    queue_enqueue(resources->reader_output_queue, item);
                    break;
                }

                item->frames_read = frames_read;
                item->stream_discontinuity_event = is_reset;
                item->is_last_chunk = false;

                if (item->frames_read > 0) {
                    pthread_mutex_lock(&resources->progress_mutex);
                    resources->total_frames_read += item->frames_read;
                    pthread_mutex_unlock(&resources->progress_mutex);
                }

                if (!queue_enqueue(resources->reader_output_queue, item)) {
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                    break;
                }
            }
            break;
        }

        case PIPELINE_MODE_REALTIME_SDR:
        case PIPELINE_MODE_FILE_PROCESSING: {
            // These modes manage their own chunking inside their start_stream functions,
            // which have already been updated to use 'pipeline_read_chunk_size'.
            ModuleContext ctx = { .config = config, .resources = resources };
            resources->selected_input_module_api->start_stream(&ctx);
            break;
        }
    }

    if (!is_shutdown_requested()) {
        log_debug("Reader thread finished naturally. End of stream reached.");
        resources->end_of_stream_reached = true;
    } else {
        SampleChunk *last_item = (SampleChunk*)queue_try_dequeue(resources->free_sample_chunk_queue);
        if (last_item) {
             last_item->is_last_chunk = true;
             last_item->frames_read = 0;
             queue_enqueue(resources->reader_output_queue, last_item);
        }
    }

    log_debug("Reader thread is exiting.");
    return NULL;
}

void* writer_thread_func(void* arg) {
    platform_set_thread_priority(PRIORITY_HIGHEST, "Writer");

    PipelineContext* args = (PipelineContext*)arg;
    ModuleContext ctx = { .config = args->config, .resources = args->resources };

    if (args->resources->selected_output_module_api && args->resources->selected_output_module_api->run_writer) {
        return args->resources->selected_output_module_api->run_writer(&ctx);
    }

    log_fatal("Writer thread started with no output module selected or run_writer is NULL.");
    return NULL;
}

void* pre_processor_thread_func(void* arg) {
    platform_set_thread_priority(PRIORITY_HIGH, "Pre-Processor");

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->pre_processor_input_queue)) != NULL) {

        if (item->is_last_chunk) {
            if (resources->iq_optimization_data_queue) {
                queue_signal_shutdown(resources->iq_optimization_data_queue);
            }
            queue_enqueue(resources->pre_processor_output_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            pre_processor_reset(resources);
            if (!queue_enqueue(resources->pre_processor_output_queue, item)) {
                break;
            }
            continue;
        }
 
        pre_processor_apply_chain(resources, item);

        if (config->dsp.iq_correction.enable) {
            if (item->frames_read >= IQ_CORRECTION_FFT_SIZE && !item->stream_discontinuity_event) {
                SampleChunk* opt_item = (SampleChunk*)queue_try_dequeue(resources->free_sample_chunk_queue);
                if (opt_item) {
                    memcpy(opt_item->complex_sample_buffer_a, item->complex_sample_buffer_a, IQ_CORRECTION_FFT_SIZE * sizeof(complex_float_t));
                    queue_enqueue(resources->iq_optimization_data_queue, opt_item);
                }
            }
        }

        if (item->frames_read > 0) {
            if (!queue_enqueue(resources->pre_processor_output_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }
        } else {
            queue_enqueue(resources->free_sample_chunk_queue, item);
        }
    }

    log_debug("Pre-processor thread is exiting.");
    return NULL;
}

void* resampler_thread_func(void* arg) {
    platform_set_thread_priority(PRIORITY_NORMAL, "Resampler");

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->resampler_input_queue)) != NULL) {
        if (item->is_last_chunk) {
            queue_enqueue(resources->resampler_output_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            resampler_reset(resources->resampler);
            if (!queue_enqueue(resources->resampler_output_queue, item)) {
                break;
            }
            continue;
        }

        // Set up the state pointers for this stage
        item->current_input_buffer = item->complex_sample_buffer_a;
        item->current_output_buffer = item->complex_sample_buffer_b;

        unsigned int output_frames_this_chunk = 0;
        if (resources->is_passthrough) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            // In passthrough, we must copy the data to the output buffer
            memcpy(item->current_output_buffer, item->current_input_buffer, output_frames_this_chunk * sizeof(complex_float_t));
        } else {
            // --- UPDATED CALL WITH CAPACITY CHECK ---
            resampler_execute(resources->resampler,
                              item->current_input_buffer,
                              (unsigned int)item->frames_read,
                              item->current_output_buffer,
                              item->complex_buffer_capacity_samples, // Pass capacity
                              &output_frames_this_chunk);
        }
        item->frames_to_write = output_frames_this_chunk;

        // --- CRITICAL PING-PONG SWAP ---
        // The output of this stage becomes the input for the next stage.
        item->current_input_buffer = item->complex_sample_buffer_b;
        item->current_output_buffer = item->complex_sample_buffer_a;

        if (!queue_enqueue(resources->resampler_output_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            break;
        }
    }
    log_debug("Resampler thread is exiting.");
    return NULL;
}

void* post_processor_thread_func(void* arg) {
    platform_set_thread_priority(PRIORITY_HIGH, "Post-Processor");

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->post_processor_input_queue)) != NULL) {

        if (item->is_last_chunk) {
            // If we are NOT using a paced buffer (e.g. stdout), we need to send the last_chunk marker to the writer.
            if (!resources->pacing_is_required) {
                queue_enqueue(resources->writer_input_queue, item);
            } else { // Otherwise, we signal the ring buffer and free the chunk.
                if (resources->writer_input_buffer) {
                    ring_buffer_signal_end_of_stream(resources->writer_input_buffer);
                }
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            break;
        }

        if (item->stream_discontinuity_event) {
            post_processor_reset(resources);
            if (!queue_enqueue(resources->post_processor_output_queue, item)) {
                break;
            }
            continue;
        }

        post_processor_apply_chain(resources, item);

        if (item->frames_to_write > 0) {
            // If we are NOT using a paced buffer, pass the chunk directly to the writer thread's queue.
            if (!resources->pacing_is_required) {
                if (!queue_enqueue(resources->writer_input_queue, item)) {
                    break;
                }
            } else { // Otherwise, write the data to the ring buffer and return the chunk to the free pool.
                if (resources->writer_input_buffer) {
                    size_t bytes_to_write = item->frames_to_write * resources->output_bytes_per_sample_pair;
                    ring_buffer_write(resources->writer_input_buffer, item->final_output_data, bytes_to_write);
                }
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
        } else {
            queue_enqueue(resources->free_sample_chunk_queue, item);
        }
    }

    log_debug("Post-processor thread is exiting.");
    return NULL;
}
