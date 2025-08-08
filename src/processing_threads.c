// processing_threads.c

#include "processing_threads.h"
#include "types.h"
#include "config.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "signal_handler.h"
#include "log.h"
#include "sample_convert.h"
#include "dc_block.h"
#include "iq_correct.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

/**
 * @brief The pre-processor thread's main function.
 */
void* pre_processor_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    // This buffer is now only needed if I/Q correction is enabled, to hold a
    // temporary copy of the already-converted samples for the optimization algorithm.
    complex_float_t temp_float_buffer[BUFFER_SIZE_SAMPLES];
    unsigned long long samples_since_last_opt = 0;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->raw_to_pre_process_queue);
        if (!item) break; // Shutdown signaled

        if (item->stream_discontinuity_event) {
            log_debug("Pre-Processor thread received reset command.");
            shift_reset_nco(resources->pre_resample_nco);
            if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            continue;
        }

        if (item->is_last_chunk) {
            queue_enqueue(resources->pre_process_to_resampler_queue, item);
            break;
        }

        // Convert raw bytes to normalized, gain-adjusted complex floats in one step.
        if (!convert_raw_to_cf32(item->raw_input_data, item->complex_pre_resample_data, item->frames_read, resources->input_format, config->gain)) {
            handle_fatal_thread_error("Pre-Processor: Failed to convert samples to complex float.", resources);
            queue_enqueue(resources->free_sample_chunk_queue, item);
            continue;
        }

        // Apply DC block filter if enabled (operates on the already converted data)
        if (config->dc_block.enable) {
            dc_block_apply(resources, item->complex_pre_resample_data, item->frames_read);
        }

        // Handle I/Q correction logic (accumulation and application)
        if (config->iq_correction.enable) {
            // We need a temporary copy for the accumulation buffer, as the main buffer
            // will be modified in-place by iq_correct_apply.
            memcpy(temp_float_buffer, item->complex_pre_resample_data, item->frames_read * sizeof(complex_float_t));

            int samples_to_process = item->frames_read;
            int offset = 0;
            while (samples_to_process > 0) {
                int samples_needed = IQ_CORRECTION_FFT_SIZE - resources->iq_correction.samples_in_accum;
                int samples_to_copy = (samples_to_process < samples_needed) ? samples_to_process : samples_needed;

                memcpy(resources->iq_correction.optimization_accum_buffer + resources->iq_correction.samples_in_accum,
                       temp_float_buffer + offset,
                       samples_to_copy * sizeof(complex_float_t));

                resources->iq_correction.samples_in_accum += samples_to_copy;
                offset += samples_to_copy;
                samples_to_process -= samples_to_copy;

                if (resources->iq_correction.samples_in_accum == IQ_CORRECTION_FFT_SIZE) {
                    if (samples_since_last_opt >= IQ_CORRECTION_DEFAULT_PERIOD) {
                        SampleChunk* opt_item = (SampleChunk*)queue_try_dequeue(resources->free_sample_chunk_queue);
                        if (opt_item) {
                            memcpy(opt_item->complex_pre_resample_data, resources->iq_correction.optimization_accum_buffer, IQ_CORRECTION_FFT_SIZE * sizeof(complex_float_t));
                            if (!queue_enqueue(resources->iq_optimization_data_queue, opt_item)) {
                                queue_enqueue(resources->free_sample_chunk_queue, opt_item);
                            }
                            samples_since_last_opt = 0;
                        } else {
                            log_debug("Skipping I/Q optimization cycle, no free buffers.");
                        }
                    }
                    resources->iq_correction.samples_in_accum = 0;
                }
            }
            samples_since_last_opt += item->frames_read;

            // Apply the correction in-place to the main buffer
            iq_correct_apply(resources, item->complex_pre_resample_data, item->frames_read);
        }

        // Apply frequency shift before resampling if configured
        if (resources->pre_resample_nco) {
            shift_apply(resources->pre_resample_nco, resources->actual_nco_shift_hz,
                        item->complex_pre_resample_data,
                        item->complex_pre_resample_data,
                        item->frames_read);
        }

        if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            break;
        }
    }
    return NULL;
}

/**
 * @brief The resampler thread's main function.
 */
void* resampler_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->pre_process_to_resampler_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
            log_debug("Resampler thread received reset command.");
            if (resources->resampler) {
                msresamp_crcf_reset(resources->resampler);
            }
            if (!queue_enqueue(resources->resampler_to_post_process_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            continue;
        }

        if (item->is_last_chunk) {
            queue_enqueue(resources->resampler_to_post_process_queue, item);
            break;
        }

        unsigned int output_frames_this_chunk = 0;
        if (resources->is_passthrough) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            memcpy(item->complex_resampled_data, item->complex_pre_resample_data, output_frames_this_chunk * sizeof(complex_float_t));
        } else {
            msresamp_crcf_execute(resources->resampler,
                                  (liquid_float_complex*)item->complex_pre_resample_data,
                                  (unsigned int)item->frames_read,
                                  (liquid_float_complex*)item->complex_resampled_data,
                                  &output_frames_this_chunk);
        }
        item->frames_to_write = output_frames_this_chunk;

        if (!queue_enqueue(resources->resampler_to_post_process_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            break;
        }
    }
    return NULL;
}

/**
 * @brief The post-processor thread's main function.
 */
void* post_processor_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->resampler_to_post_process_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
            log_debug("Post-Processor thread received reset command.");
            shift_reset_nco(resources->post_resample_nco);
            if (!queue_enqueue(resources->final_output_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            continue;
        }

        if (item->is_last_chunk) {
            queue_enqueue(resources->final_output_queue, item);
            break;
        }

        complex_float_t* final_complex_data = item->complex_resampled_data;

        if (resources->post_resample_nco) {
            shift_apply(resources->post_resample_nco, resources->actual_nco_shift_hz,
                        item->complex_resampled_data,
                        item->complex_post_resample_data,
                        item->frames_to_write);
            final_complex_data = item->complex_post_resample_data;
        }

        if (!convert_cf32_to_block(final_complex_data, item->final_output_data, item->frames_to_write, config->output_format)) {
            handle_fatal_thread_error("Post-Processor: Failed to convert samples to output format.", resources);
            queue_enqueue(resources->free_sample_chunk_queue, item);
            break;
        }

        if (!queue_enqueue(resources->final_output_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            break;
        }
    }
    return NULL;
}

/**
 * @brief The I/Q optimization thread's main function.
 */
void* iq_optimization_thread_func(void* arg) {
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->iq_optimization_data_queue);
        if (!item) break;

        iq_correct_run_optimization(resources, item->complex_pre_resample_data);
        queue_enqueue(resources->free_sample_chunk_queue, item);
    }
    return NULL;
}
