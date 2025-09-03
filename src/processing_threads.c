#include "processing_threads.h"
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
#include "file_write_buffer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

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
    while ((item = (SampleChunk*)queue_dequeue(resources->raw_to_pre_process_queue)) != NULL) {

        if (item->is_last_chunk) {
            if (resources->iq_optimization_data_queue) {
                queue_signal_shutdown(resources->iq_optimization_data_queue);
            }
            queue_enqueue(resources->pre_process_to_resampler_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            pre_processor_reset(resources);
            if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
                break;
            }
            continue;
        }

        pre_processor_apply_chain(resources, item);

        // Send a copy of the pre-processed data to the optimization thread for training.
        if (config->iq_correction.enable) {
            if (item->frames_read >= IQ_CORRECTION_FFT_SIZE && !item->stream_discontinuity_event) {
                SampleChunk* opt_item = (SampleChunk*)queue_try_dequeue(resources->free_sample_chunk_queue);
                if (opt_item) {
                    memcpy(opt_item->complex_pre_resample_data, item->complex_pre_resample_data, IQ_CORRECTION_FFT_SIZE * sizeof(complex_float_t));
                    queue_enqueue(resources->iq_optimization_data_queue, opt_item);
                }
            }
        }

        if (item->frames_read > 0) {
            if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
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
    while ((item = (SampleChunk*)queue_dequeue(resources->pre_process_to_resampler_queue)) != NULL) {
        if (item->is_last_chunk) {
            queue_enqueue(resources->resampler_to_post_process_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            resampler_reset(resources->resampler);
            if (!queue_enqueue(resources->resampler_to_post_process_queue, item)) {
                break;
            }
            continue;
        }

        unsigned int output_frames_this_chunk = 0;
        if (resources->is_passthrough) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            memcpy(item->complex_resampled_data, item->complex_pre_resample_data, output_frames_this_chunk * sizeof(complex_float_t));
        } else {
            resampler_execute(resources->resampler, item->complex_pre_resample_data, (unsigned int)item->frames_read, item->complex_resampled_data, &output_frames_this_chunk);
        }
        item->frames_to_write = output_frames_this_chunk;

        if (!queue_enqueue(resources->resampler_to_post_process_queue, item)) {
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
    while ((item = (SampleChunk*)queue_dequeue(resources->resampler_to_post_process_queue)) != NULL) {
 
        if (item->is_last_chunk) {
            if (config->output_to_stdout) {
                queue_enqueue(resources->stdout_queue, item);
            } else {
                file_write_buffer_signal_end_of_stream(resources->file_write_buffer);
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            break;
        }

        if (item->stream_discontinuity_event) {
            post_processor_reset(resources);
            if (config->output_to_stdout) {
                if (!queue_enqueue(resources->stdout_queue, item)) {
                    break;
                }
            } else {
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            continue;
        }

        post_processor_apply_chain(resources, item);

        if (item->frames_to_write > 0) {
            if (config->output_to_stdout) {
                if (!queue_enqueue(resources->stdout_queue, item)) {
                    break;
                }
            } else {
                size_t bytes_to_write = item->frames_to_write * resources->output_bytes_per_sample_pair;
                file_write_buffer_write(resources->file_write_buffer, item->final_output_data, bytes_to_write);
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
        } else {
            queue_enqueue(resources->free_sample_chunk_queue, item);
        }
    }

    log_debug("Post-processor thread is exiting.");
    return NULL;
}

void* iq_optimization_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->iq_optimization_data_queue)) != NULL) {
        iq_correct_run_optimization(resources, item->complex_pre_resample_data);
        queue_enqueue(resources->free_sample_chunk_queue, item);
    }
    log_debug("I/Q optimization thread is exiting.");
    return NULL;
}
