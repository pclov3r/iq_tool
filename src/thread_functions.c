/**
 * @file thread_functions.c
 * @brief Implements the entry-point functions for all application data pipeline threads.
 */

#ifdef _WIN32
#include <windows.h>
#endif

#include "thread_functions.h"
#include "pipeline_context.h"
#include "constants.h"
#include "app_context.h"
#include "utils.h"
#include "signal_handler.h"
#include "log.h"
#include "pre_processor.h"
#include "post_processor.h"
#include "iq_correct.h"
#include "resampler.h"
#include "queue.h"
#include "ring_buffer.h"
#include "sdr_packet_serializer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>


// --- I/O Thread Functions ---

void* sdr_capture_thread_func(void* arg) {
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        log_warn("Failed to set SDR capture thread priority to TIME_CRITICAL.");
    }
#endif
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
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL)) {
        log_warn("Failed to set reader thread priority.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    switch (resources->pipeline_mode) {
        case PIPELINE_MODE_BUFFERED_SDR: {
            log_debug("Reader thread starting in buffered SDR mode.");

            while (!is_shutdown_requested() && !resources->error_occurred) {
                SampleChunk* item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (!item) break;

                bool is_reset = false;
                
                int64_t frames_read = sdr_packet_serializer_read_packet(
                    resources->sdr_input_buffer,
                    item,
                    &is_reset,
                    resources->sdr_deserializer_temp_buffer,
                    resources->sdr_deserializer_buffer_size
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
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        log_warn("Failed to set writer thread priority to HIGHEST.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    if (config->output_to_stdout) {
        while (true) {
            SampleChunk* item = (SampleChunk*)queue_dequeue(resources->writer_input_queue);
            if (!item) break;

            if (item->stream_discontinuity_event) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                continue;
            }

            if (item->is_last_chunk) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }

            size_t output_bytes_this_chunk = item->frames_to_write * resources->output_bytes_per_sample_pair;
            if (output_bytes_this_chunk > 0) {
                size_t written_bytes = resources->writer_ctx.api.write(&resources->writer_ctx, item->final_output_data, output_bytes_this_chunk);
                if (written_bytes != output_bytes_this_chunk) {
                    if (!is_shutdown_requested()) {
                        log_debug("Writer: stdout write error: %s", strerror(errno));
                        request_shutdown();
                    }
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                    break;
                }
            }
            
            if (!queue_enqueue(resources->free_sample_chunk_queue, item)) {
                break;
            }
        }
    } else {
        unsigned char* local_write_buffer = (unsigned char*)resources->writer_local_buffer;
        if (!local_write_buffer) {
            handle_fatal_thread_error("Writer: Local write buffer is NULL.", resources);
            return NULL;
        }

        while (true) {
            size_t bytes_read = ring_buffer_read(resources->writer_input_buffer, local_write_buffer, IO_OUTPUT_WRITER_CHUNK_SIZE);

            if (bytes_read == 0) {
                break;
            }

            size_t written_bytes = resources->writer_ctx.api.write(&resources->writer_ctx, local_write_buffer, bytes_read);
            
            if (written_bytes != bytes_read) {
                char error_buf[256];
                snprintf(error_buf, sizeof(error_buf), "Writer: File write error: %s", strerror(errno));
                handle_fatal_thread_error(error_buf, resources);
                break;
            }

            if (resources->progress_callback) {
                long long current_bytes = resources->writer_ctx.api.get_total_bytes_written(&resources->writer_ctx);
                unsigned long long current_frames = current_bytes / resources->output_bytes_per_sample_pair;
                
                pthread_mutex_lock(&resources->progress_mutex);
                resources->total_output_frames = current_frames;
                pthread_mutex_unlock(&resources->progress_mutex);

                resources->progress_callback(current_frames, resources->expected_total_output_frames, current_bytes, resources->progress_callback_udata);
            }
        }
    }

    log_debug("Writer thread is exiting.");
    return NULL;
}


// --- DSP Thread Functions ---

void* pre_processor_thread_func(void* arg) {
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        log_warn("Failed to set pre-processor thread priority.");
    }
#endif

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

        if (config->iq_correction.enable) {
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
            resampler_execute(resources->resampler, item->current_input_buffer, (unsigned int)item->frames_read, item->current_output_buffer, &output_frames_this_chunk);
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
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        log_warn("Failed to set post-processor thread priority.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->post_processor_input_queue)) != NULL) {
 
        if (item->is_last_chunk) {
            if (config->output_to_stdout) {
                queue_enqueue(resources->writer_input_queue, item);
            } else {
                ring_buffer_signal_end_of_stream(resources->writer_input_buffer);
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
            if (config->output_to_stdout) {
                if (!queue_enqueue(resources->writer_input_queue, item)) {
                    break;
                }
            } else {
                size_t bytes_to_write = item->frames_to_write * resources->output_bytes_per_sample_pair;
                ring_buffer_write(resources->writer_input_buffer, item->final_output_data, bytes_to_write);
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
        } else {
            queue_enqueue(resources->free_sample_chunk_queue, item);
        }
    }

    log_debug("Post-processor thread is exiting.");
    return NULL;
}


// --- Utility Thread Functions ---

void* iq_optimization_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->iq_optimization_data_queue)) != NULL) {
        iq_correct_run_optimization(resources, item->complex_sample_buffer_a);
        queue_enqueue(resources->free_sample_chunk_queue, item);
    }
    log_debug("I/Q optimization thread is exiting.");
    return NULL;
}
