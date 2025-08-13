// src/io_threads.c

#ifdef _WIN32
#include <windows.h>
#endif

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

void* reader_thread_func(void* arg) {
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        log_warn("Failed to set reader thread priority to TIME_CRITICAL.");
    }
    DWORD_PTR affinity_mask = (1 << 2);
    if (SetThreadAffinityMask(GetCurrentThread(), affinity_mask) == 0) {
        log_warn("Failed to set reader thread affinity mask.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;
    InputSourceContext ctx = { .config = config, .resources = resources };

    // The start_stream function for the input source (e.g., wav_start_stream)
    // will block until the file is read, and it is responsible for enqueuing
    // the final "is_last_chunk = true" marker.
    resources->selected_input_ops->start_stream(&ctx);

    // This code is reached only after the stream has stopped naturally (e.g., end of file).
    if (!is_shutdown_requested()) {
        // The premature request_shutdown() and redundant marker have been removed.
        // This thread's only job now is to set a flag for the final summary report
        // and then exit, allowing the pipeline to drain based on the marker.
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
    DWORD_PTR affinity_mask = ~(1 << 2);
    if (SetThreadAffinityMask(GetCurrentThread(), affinity_mask) == 0) {
        log_warn("Failed to set writer thread affinity mask.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;
    int loop_count = 0;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->final_output_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            continue;
        }

        // The "last chunk" marker is now the definitive signal to exit.
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

        if (item->frames_to_write > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_output_frames += item->frames_to_write;
            pthread_mutex_unlock(&resources->progress_mutex);

            loop_count++;
            if (!config->output_to_stdout && (loop_count % PROGRESS_UPDATE_INTERVAL == 0)) {
                if (resources->progress_callback) {
                    // --- MODIFIED: Call the callback with the writer's progress ---
                    resources->progress_callback(resources->total_output_frames, resources->expected_total_output_frames, 0, resources->progress_callback_udata);
                }
            }
        }

        if (!queue_enqueue(resources->free_sample_chunk_queue, item)) {
            break;
        }
    }
    log_debug("Writer thread is exiting.");
    return NULL;
}
