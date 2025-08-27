#ifdef _WIN32
#include <windows.h>
#endif

#include "io_threads.h"
#include "constants.h"
#include "app_context.h"
#include "signal_handler.h"
#include "log.h"
#include "input_source.h"
#include "queue.h"
#include "sdr_packet_serializer.h"
#include "pipeline_context.h"
#include "file_write_buffer.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

void* sdr_capture_thread_func(void* arg) {
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        log_warn("Failed to set SDR capture thread priority to TIME_CRITICAL.");
    }
#endif
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    InputSourceContext ctx = { .config = args->config, .resources = resources };

    // This is a blocking call that runs the SDR hardware's main loop.
    // The SDR's callback function is now responsible for writing framed packets
    // into the sdr_input_buffer.
    resources->selected_input_ops->start_stream(&ctx);

    // When start_stream returns, it means the SDR has been stopped (usually by shutdown).
    // We must signal to the reader_thread that no more data will ever be written.
    if (resources->sdr_input_buffer) {
        file_write_buffer_signal_end_of_stream(resources->sdr_input_buffer);
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
                if (!item) break; // Shutdown signaled

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
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                    break; // Exit the loop on clean end-of-stream.
                }

                item->frames_read = frames_read;
                item->stream_discontinuity_event = is_reset;
                item->is_last_chunk = false;

                if (item->frames_read > 0) {
                    pthread_mutex_lock(&resources->progress_mutex);
                    resources->total_frames_read += item->frames_read;
                    pthread_mutex_unlock(&resources->progress_mutex);
                }

                if (!queue_enqueue(resources->raw_to_pre_process_queue, item)) {
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                    break; // Shutdown while trying to enqueue.
                }
            }

            if (!is_shutdown_requested()) {
                SampleChunk *last_item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (last_item) {
                    last_item->is_last_chunk = true;
                    last_item->frames_read = 0;
                    queue_enqueue(resources->raw_to_pre_process_queue, last_item);
                }
            }
            break;
        }

        case PIPELINE_MODE_REALTIME_SDR:
        case PIPELINE_MODE_FILE_PROCESSING: {
            InputSourceContext ctx = { .config = config, .resources = resources };
            resources->selected_input_ops->start_stream(&ctx);
            break;
        }
    }

    if (!is_shutdown_requested()) {
        log_debug("Reader thread finished naturally. End of stream reached.");
        resources->end_of_stream_reached = true;
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
            SampleChunk* item = (SampleChunk*)queue_dequeue(resources->stdout_queue);
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
                size_t written_bytes = resources->writer_ctx.ops.write(&resources->writer_ctx, item->final_output_data, output_bytes_this_chunk);
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
            size_t bytes_read = file_write_buffer_read(resources->file_write_buffer, local_write_buffer, IO_FILE_WRITER_CHUNK_SIZE);

            if (bytes_read == 0) {
                break; // End of stream or shutdown
            }

            size_t written_bytes = resources->writer_ctx.ops.write(&resources->writer_ctx, local_write_buffer, bytes_read);
            
            if (written_bytes != bytes_read) {
                char error_buf[256];
                snprintf(error_buf, sizeof(error_buf), "Writer: File write error: %s", strerror(errno));
                handle_fatal_thread_error(error_buf, resources);
                break;
            }

            if (resources->progress_callback) {
                long long current_bytes = resources->writer_ctx.ops.get_total_bytes_written(&resources->writer_ctx);
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
