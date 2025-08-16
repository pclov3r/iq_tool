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

    resources->selected_input_ops->start_stream(&ctx);

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
    DWORD_PTR affinity_mask = ~(1 << 2);
    if (SetThreadAffinityMask(GetCurrentThread(), affinity_mask) == 0) {
        log_warn("Failed to set writer thread affinity mask.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;
    int loop_count = 0;

    if (config->output_to_stdout) {
        // --- STDOUT PATH: Use the simple, low-memory, tightly-coupled queue ---
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
        // --- FILE PATH: Use the robust, decoupled I/O buffer ---
        unsigned char* local_write_buffer = (unsigned char*)malloc(WRITER_THREAD_CHUNK_SIZE);
        if (!local_write_buffer) {
            handle_fatal_thread_error("Writer: Failed to allocate local write buffer.", resources);
            return NULL;
        }

        while (true) {
            size_t bytes_read = file_write_buffer_read(resources->file_write_buffer, local_write_buffer, WRITER_THREAD_CHUNK_SIZE);

            if (bytes_read == 0) {
                break;
            }

            size_t written_bytes = resources->writer_ctx.ops.write(&resources->writer_ctx, local_write_buffer, bytes_read);
            
            if (written_bytes != bytes_read) {
                char error_buf[256];
                snprintf(error_buf, sizeof(error_buf), "Writer: File write error: %s", strerror(errno));
                handle_fatal_thread_error(error_buf, resources);
                break;
            }

            loop_count++;
            if (loop_count % PROGRESS_UPDATE_INTERVAL == 0) {
                if (resources->progress_callback) {
                    long long current_bytes = resources->writer_ctx.ops.get_total_bytes_written(&resources->writer_ctx);
                    unsigned long long current_frames = current_bytes / resources->output_bytes_per_sample_pair;
                    
                    pthread_mutex_lock(&resources->progress_mutex);
                    resources->total_output_frames = current_frames;
                    pthread_mutex_unlock(&resources->progress_mutex);

                    resources->progress_callback(current_frames, resources->expected_total_output_frames, 0, resources->progress_callback_udata);
                }
            }
        }
        free(local_write_buffer);
    }

    log_debug("Writer thread is exiting.");
    return NULL;
}
