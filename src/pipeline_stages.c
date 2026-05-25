/**
 * @file pipeline_stages.c
 * @brief Implements the high-speed data processing stages for the DSP pipeline.
 */

#include "pipeline_stages.h"
#include "pipeline_context.h"
#include "app_context.h"
#include "constants.h"
#include "log.h"
#include "utils.h"
#include "platform.h"
#include "signal_handler.h"
#include "queue.h"
#include "ring_buffer.h"
#include "packet_serializer.h"
#include "input_common.h"
#include "pre_processor.h"
#include "post_processor.h"
#include "resampler.h"
#include "module_registry.h"
#include "iq_correction.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// --- Pipeline Thread Function Implementations (Private to this module) ---
// Universal Ingest Callback
static bool pipeline_queue_samples(void* ctx, const void* data, size_t num_samples, SampleFormat format) {
    AppContext* app = (AppContext*)ctx;
    source_update_heartbeat(app);
    if (is_shutdown_requested() || atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed)) return false;

    if (!packet_serializer_write_block(app->pipeline.source_input_buffer, num_samples, data, format)) {
        static double last_drop_log_time = 0.0;
        static size_t accumulated_drops = 0;
        
        accumulated_drops += num_samples;
        double current_time = utils_get_time();
        
        if (current_time - last_drop_log_time >= CONSOLE_UPDATE_INTERVAL_SEC) {
            log_warn("Pipeline input overrun! Dropped %zu samples.", accumulated_drops);
            accumulated_drops = 0;
            last_drop_log_time = current_time;
        }
        return false;
    }
    return true;
}

void* pipeline_thread_source(void* arg) {
    platform_set_thread_priority(PRIORITY_REALTIME, "Source");

    PipelineContext* args = (PipelineContext*)arg;
    AppContext* app = args->app;
    ModuleContext ctx = { .config = args->config, .app = app };

    app->module.input_api->start_stream(&ctx, pipeline_queue_samples, app);

    if (app->pipeline.source_input_buffer) {
        ring_buffer_signal_end_of_stream(app->pipeline.source_input_buffer);
    }

    log_debug("Source capture thread is exiting.");
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
                    app->pipeline.source_input_buffer,
                    item,
                    &state,
                    &is_reset,
                    app->pipeline.read_chunk_size
                );

                if (frames_read < 0) {
                    handle_fatal_thread_error("Reader: Fatal error parsing source buffer stream.", app);
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
                    size_t bytes = item->frames_read * item->input_bytes_per_iq_sample;
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

                    size_t bytes_requested = app->pipeline.read_chunk_size * app->module.input_bytes_per_iq_sample;
                    size_t capacity_bytes = item->raw_input_capacity_bytes;
                    if (bytes_requested > capacity_bytes) bytes_requested = capacity_bytes;

                    void* target_buffer = config->dsp.raw_passthrough ? item->final_output_data : item->raw_input_data;
                    size_t bytes_read = in_api->read_chunk(&ctx, target_buffer, bytes_requested);

                    item->frames_read = bytes_read / app->module.input_bytes_per_iq_sample;
                    item->frames_to_write = (unsigned int)item->frames_read;
                    item->packet_sample_format = app->module.input_format;
                    item->input_bytes_per_iq_sample = app->module.input_bytes_per_iq_sample;
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
        log_error("Output module does not implement write_chunk.");
        return NULL;
    }

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(app->pipeline.writer_input_queue);
        if (!item) break; // Shutdown signaled

        // 1. Process payload FIRST if it exists
        // This ensures the partial "tail" of a file is written before we exit.
        if (item->frames_to_write > 0 && !item->stream_discontinuity_event) {
            size_t bytes_to_write = item->frames_to_write * app->module.output_bytes_per_iq_sample;
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

        // 2. Handle Reset Event (Source Overrun)
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

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(app->pipeline.pre_processor_input_queue)) != NULL) {
        bool is_last = item->is_last_chunk;

        if (item->stream_discontinuity_event) {
            pre_processor_reset(&app->dsp);
            if (!queue_enqueue(app->pipeline.pre_processor_output_queue, item)) {
                break;
            }
            continue;
        }

        if (item->frames_read > 0) {
            pre_processor_apply_chain(&app->dsp, item);
        }

        if (app->dsp.bypass_resampler) {
            item->frames_to_write = (unsigned int)item->frames_read;
            // Copy data to the post-resample buffer since the resampler thread is bypassed
            memcpy(item->post_resample_buffer, item->pre_resample_buffer, item->frames_to_write * sizeof(ComplexFloat));
        }

        if (!queue_enqueue(app->pipeline.pre_processor_output_queue, item)) {
            queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
            break;
        }

        if (is_last) {
            if (app->pipeline.iq_optimization_data_queue) {
                queue_signal_shutdown(app->pipeline.iq_optimization_data_queue);
            }
            break;
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
        bool is_last = item->is_last_chunk;

        if (item->stream_discontinuity_event) {
            resampler_reset(app->dsp.resampler);
            if (!queue_enqueue(app->pipeline.resampler_output_queue, item)) {
                break;
            }
            continue;
        }

        unsigned int output_frames_this_chunk = 0;
        if (app->dsp.bypass_resampler) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            memcpy(item->post_resample_buffer, item->pre_resample_buffer, output_frames_this_chunk * sizeof(ComplexFloat));
        } else if (item->frames_read > 0) {
            // Estimate maximum output length mathematically prior to execution
            unsigned int estimated_out = (unsigned int)((item->frames_read + 32) * app->dsp.resample_ratio) + 64;
            if (estimated_out > item->complex_buffer_capacity_samples) {
                handle_fatal_thread_error("Resampler input chunk is too large for ping-pong buffers!", app);
                break;
            }
            resampler_execute(app->dsp.resampler,
                              item->pre_resample_buffer,
                              (unsigned int)item->frames_read,
                              item->post_resample_buffer,
                              &output_frames_this_chunk);
        }
        item->frames_to_write = output_frames_this_chunk;

        if (!queue_enqueue(app->pipeline.resampler_output_queue, item)) {
            queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
            break;
        }

        if (is_last) {
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
        bool is_last = item->is_last_chunk;

        if (item->stream_discontinuity_event) {
            post_processor_reset(&app->dsp);
            if (!queue_enqueue(app->pipeline.post_processor_output_queue, item)) {
                break;
            }
            continue;
        }

        if (item->frames_to_write > 0) {
            post_processor_apply_chain(&app->dsp, item);
        }

        if (!queue_enqueue(app->pipeline.writer_input_queue, item)) {
            queue_enqueue_forced(app->pipeline.free_sample_chunk_queue, item);
            break;
        }

        if (is_last) {
            break;
        }
    }

    log_debug("Post-processor thread is exiting.");
    return NULL;
}
