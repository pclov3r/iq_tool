// io_threads.c

#include "io_threads.h"
#include "types.h"
#include "config.h"
#include "signal_handler.h"
#include "log.h"
#include "input_source.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

/**
 * @brief The reader thread's main function.
 *        Reads from the input source and places raw data into the pipeline.
 */
void* reader_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    InputSourceContext ctx = { .config = args->config, .resources = resources };

    // This call blocks until the input source is finished or a shutdown is signaled.
    resources->selected_input_ops->start_stream(&ctx);

    if (!is_shutdown_requested()) {
        log_debug("Reader thread finished naturally. Initiating pipeline shutdown.");
        resources->end_of_stream_reached = true;
        request_shutdown();
    }

    // This call stops the hardware stream if it's still running (e.g., on shutdown).
    // For file-based inputs, this is a no-op.
    resources->selected_input_ops->stop_stream(&ctx);

    return NULL;
}

/**
 * @brief The writer thread's main function.
 *        Writes the final processed data to the output file or stdout.
 */
void* writer_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;
    int loop_count = 0;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->final_output_queue);
        if (!item) break; // Shutdown signaled

        if (item->stream_discontinuity_event) {
            log_debug("Writer thread received and consumed reset command.");
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
                if (config->output_to_stdout) {
                    if (!is_shutdown_requested()) {
                        log_info("Downstream pipe closed. Shutting down.");
                        request_shutdown();
                    }
                } else {
                    char error_buf[256];
                    snprintf(error_buf, sizeof(error_buf), "Writer: File write error: %s", strerror(errno));
                    handle_fatal_thread_error(error_buf, resources);
                }
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }
        }

        // Update progress counters
        if (item->frames_to_write > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_output_frames += item->frames_to_write;
            unsigned long long current_total_read = resources->total_frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);

            loop_count++;
            if (!config->output_to_stdout && (loop_count % PROGRESS_UPDATE_INTERVAL == 0)) {
                if (resources->progress_callback) {
                    resources->progress_callback(current_total_read, resources->source_info.frames, resources->total_output_frames, resources->progress_callback_udata);
                }
            }
        }

        // Return the chunk to the free pool
        if (!queue_enqueue(resources->free_sample_chunk_queue, item)) {
            break;
        }
    }
    return NULL;
}
