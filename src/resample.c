// resample.c
#define _POSIX_C_SOURCE 200809L

#include "resample.h"
#include "setup.h"
#include "types.h"
#include "config.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "signal_handler.h" // For is_shutdown_requested and handle_fatal_thread_error
#include "log.h"
#include "sample_convert.h"
#include "input_source.h"
#include "dc_block.h"
#include "iq_correct.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>

#ifdef _WIN32
#include <liquid.h>
#include <windows.h> // For Sleep()
#else
#include <liquid/liquid.h>
#include <time.h>    // For nanosleep()
#endif

extern pthread_mutex_t g_console_mutex;

typedef struct {
    AppConfig* config;
    AppResources* resources;
} ThreadArgs;

static void* reader_thread_func(void* arg);
static void* processor_thread_func(void* arg);
static void* writer_thread_func(void* arg);

bool run_processing_threads(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    static ThreadArgs thread_args;
    thread_args.config = config;
    thread_args.resources = resources;

    if (pthread_create(&resources->reader_thread, NULL, reader_thread_func, &thread_args) != 0 ||
        pthread_create(&resources->processor_thread, NULL, processor_thread_func, &thread_args) != 0 ||
        pthread_create(&resources->writer_thread, NULL, writer_thread_func, &thread_args) != 0) {
        handle_fatal_thread_error("In Main: Failed to create one or more threads.", resources);
        return false;
    }

    return true;
}

static void* reader_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    InputSourceContext ctx = { .config = args->config, .resources = args->resources };
    args->resources->selected_input_ops->start_stream(&ctx);
    args->resources->selected_input_ops->stop_stream(&ctx);
    return NULL;
}

static void* processor_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;
    const int samples_for_analysis = IQ_CORRECTION_FFT_SIZE * IQ_CORRECTION_FFT_COUNT;

    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->input_q);
        if (!item) break;
        if (item->is_last_chunk) {
            queue_enqueue(resources->output_q, item);
            break;
        }

        if (!convert_block_to_cf32(item->raw_input_buffer, item->complex_buffer_scaled, item->frames_read, resources->input_format, config->gain)) {
            handle_fatal_thread_error("In Processor thread: Failed to convert samples due to unhandled format.", resources);
            queue_enqueue(resources->free_pool_q, item);
            break;
        }

        // Apply DC Block FIRST (if enabled)
        if (config->dc_block.enable) {
            pthread_mutex_lock(&resources->dsp_mutex); // Protects dc_block_filter state
            dc_block_apply(resources, item->complex_buffer_scaled, item->frames_read);
            pthread_mutex_unlock(&resources->dsp_mutex);
        }

        // Now, I/Q Correction (both optimization and application) operates on DC-blocked data.
        if (config->iq_correction.enable) {
            pthread_mutex_lock(&resources->dsp_mutex); // Protects IQ correction DSP state

            // Accumulate samples for I/Q optimization from the DC-blocked buffer
            int samples_to_copy = item->frames_read;
            unsigned long long remaining_space = (unsigned long long)IQ_CORRECTION_DEFAULT_PERIOD - resources->iq_correction.samples_accumulated_for_optimize;

            if ((unsigned long long)samples_to_copy > remaining_space) {
                samples_to_copy = (int)remaining_space;
            }

            if (samples_to_copy > 0) {
                memcpy(resources->iq_correction.optimization_accum_buffer + resources->iq_correction.samples_accumulated_for_optimize,
                       item->complex_buffer_scaled, // This buffer now contains DC-blocked data
                       samples_to_copy * sizeof(complex_float_t));
                resources->iq_correction.samples_accumulated_for_optimize += samples_to_copy;
            }

            // Run optimization if enough samples accumulated
            if (resources->iq_correction.samples_accumulated_for_optimize >= (unsigned long long)IQ_CORRECTION_DEFAULT_PERIOD) {
                const complex_float_t* analysis_data_start = resources->iq_correction.optimization_accum_buffer +
                                                             resources->iq_correction.samples_accumulated_for_optimize -
                                                             samples_for_analysis;
                iq_correct_run_optimization(resources, analysis_data_start, samples_for_analysis);
                resources->iq_correction.samples_accumulated_for_optimize = 0;
            }

            // Apply I/Q Correction to the DC-blocked data
            iq_correct_apply(resources, item->complex_buffer_scaled, item->frames_read);

            pthread_mutex_unlock(&resources->dsp_mutex); // Release mutex after all IQ/DC ops
        }

        complex_float_t *input_for_resampler_stage = item->complex_buffer_scaled;
        if (resources->shifter_nco != NULL && !config->shift_after_resample) {
            pthread_mutex_lock(&resources->dsp_mutex);
            shift_apply(config, resources, item, SHIFT_STAGE_PRE_RESAMPLE);
            pthread_mutex_unlock(&resources->dsp_mutex);
            input_for_resampler_stage = item->complex_buffer_shifted;
        }

        unsigned int output_frames_this_chunk = 0;
        pthread_mutex_lock(&resources->dsp_mutex);
        if (resources->is_passthrough) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            memcpy(item->complex_buffer_resampled, input_for_resampler_stage, output_frames_this_chunk * sizeof(complex_float_t));
        } else {
            msresamp_crcf_execute(resources->resampler,
                                  (liquid_float_complex*)input_for_resampler_stage,
                                  (unsigned int)item->frames_read,
                                  (liquid_float_complex*)item->complex_buffer_resampled,
                                  &output_frames_this_chunk);
        }
        pthread_mutex_unlock(&resources->dsp_mutex);
        item->frames_to_write = output_frames_this_chunk;

        complex_float_t *final_complex_data = item->complex_buffer_resampled;
        if (resources->shifter_nco != NULL && config->shift_after_resample) {
            pthread_mutex_lock(&resources->dsp_mutex);
            shift_apply(config, resources, item, SHIFT_STAGE_POST_RESAMPLE);
            pthread_mutex_unlock(&resources->dsp_mutex);
            final_complex_data = item->complex_buffer_shifted;
        }

        if (!convert_cf32_to_block(final_complex_data, item->output_buffer, item->frames_to_write, config->output_format)) {
            handle_fatal_thread_error("In Processor thread: Failed to convert samples to output format.", resources);
            queue_enqueue(resources->free_pool_q, item);
            break;
        }

        if (!queue_enqueue(resources->output_q, item)) {
             queue_enqueue(resources->free_pool_q, item);
             break;
        }
    }
    return NULL;
}

static void* writer_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;
    int loop_count = 0;

    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->output_q);
        if (!item) break;
        if (item->is_last_chunk) {
            queue_enqueue(resources->free_pool_q, item);
            break;
        }
        size_t output_bytes_this_chunk = item->frames_to_write * resources->output_bytes_per_sample_pair;
        if (output_bytes_this_chunk > 0) {
            size_t written_bytes = resources->writer_ctx.ops.write(&resources->writer_ctx, item->output_buffer, output_bytes_this_chunk);

            if (written_bytes != output_bytes_this_chunk) {
                if (config->output_to_stdout) {
                    if (!is_shutdown_requested()) {
                        log_info("Downstream pipe closed. Shutting down.");
                        request_shutdown();
                    }
                } else {
                    char error_buf[256];
                    snprintf(error_buf, sizeof(error_buf), "In Writer thread: File write error: %s", strerror(errno));
                    handle_fatal_thread_error(error_buf, resources);
                }
                queue_enqueue(resources->free_pool_q, item);
                break;
            }
        }
        if (item->frames_to_write > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_output_frames += item->frames_to_write;
            unsigned long long current_total_read = resources->total_frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);

            loop_count++;
            if (!config->output_to_stdout && (loop_count % PROGRESS_UPDATE_INTERVAL == 0)) {
                if (resources->progress_callback) {
                    resources->progress_callback(current_total_read,
                                                resources->source_info.frames,
                                                resources->total_output_frames,
                                                resources->progress_callback_udata);
                }
            }
        }
        if (!queue_enqueue(resources->free_pool_q, item)) {
             break;
        }
    }
    return NULL;
}
