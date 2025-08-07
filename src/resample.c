#include "resample.h"
#include "setup.h"
#include "types.h"
#include "config.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "signal_handler.h"
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

// Add an external declaration for the global console mutex defined in main.c
extern pthread_mutex_t g_console_mutex;

// --- Private struct to pass arguments to threads ---
typedef struct {
    AppConfig* config;
    AppResources* resources;
} ThreadArgs;

// --- Forward declarations for thread functions ---
static void* reader_thread_func(void* arg);
static void* processor_thread_func(void* arg);
static void* writer_thread_func(void* arg);


/**
 * @brief Public entry point to start the processing pipeline.
 *        Creates, runs, and joins the reader, processor, and writer threads.
 */
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

/**
 * @brief The reader thread's main function.
 *        It calls the input source's streaming functions and blocks until the
 *        stream ends or a shutdown is requested.
 */
static void* reader_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    InputSourceContext ctx = { .config = args->config, .resources = args->resources };
    // This call blocks until the input source is finished or a shutdown is signaled
    args->resources->selected_input_ops->start_stream(&ctx);
    // This call stops the hardware stream if it's still running (e.g., on shutdown)
    args->resources->selected_input_ops->stop_stream(&ctx);
    return NULL;
}

/**
 * @brief The processor thread's main function.
 *        This is the core of the DSP pipeline. It dequeues raw data, performs
 *        all necessary corrections, conversions, and resampling, then enqueues
 *        the result for the writer thread.
 */
static void* processor_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;

    // This buffer is only used for integer-format inputs.
    complex_float_t temp_unnormalized_float_buffer[BUFFER_SIZE_SAMPLES];

    unsigned long long samples_since_last_opt = 0;

    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->input_q);
        if (!item) break; // NULL item signals shutdown
        if (item->is_last_chunk) {
            queue_enqueue(resources->output_q, item); // Pass the final chunk marker to the writer
            break;
        }

        // --- PRE-PROCESSING STAGE ---
        if (resources->input_format == CF32) {
            // --- PATH FOR CF32 (Complex Float) INPUT ---
            const complex_float_t* in_cf32 = (const complex_float_t*)item->raw_input_buffer;
            for (int i = 0; i < item->frames_read; i++) {
                item->complex_buffer_scaled[i] = in_cf32[i] * config->gain;
            }
        } else {
            // --- PATH FOR ALL INTEGER INPUTS (cs8, cu16, etc.) ---

            // Step 1: Convert raw integer samples to un-normalized floats in our dedicated temp buffer.
            switch (resources->input_format) {
                case CS8: {
                    const int8_t* in = (const int8_t*)item->raw_input_buffer;
                    for (int i = 0; i < item->frames_read; i++) { temp_unnormalized_float_buffer[i] = (float)in[i*2] + I * (float)in[i*2+1]; }
                    break;
                }
                case CU8: {
                    const uint8_t* in = (const uint8_t*)item->raw_input_buffer;
                    for (int i = 0; i < item->frames_read; i++) { temp_unnormalized_float_buffer[i] = ((float)in[i*2] - 127.5f) + I * ((float)in[i*2+1] - 127.5f); }
                    break;
                }
                case CS16: {
                    const int16_t* in = (const int16_t*)item->raw_input_buffer;
                    for (int i = 0; i < item->frames_read; i++) { temp_unnormalized_float_buffer[i] = (float)in[i*2] + I * (float)in[i*2+1]; }
                    break;
                }
                case CU16: {
                    const uint16_t* in = (const uint16_t*)item->raw_input_buffer;
                    for (int i = 0; i < item->frames_read; i++) { temp_unnormalized_float_buffer[i] = ((float)in[i*2] - 32767.5f) + I * ((float)in[i*2+1] - 32767.5f); }
                    break;
                }
                case CS32: {
                    const int32_t* in = (const int32_t*)item->raw_input_buffer;
                    for (int i = 0; i < item->frames_read; i++) { temp_unnormalized_float_buffer[i] = (float)in[i*2] + I * (float)in[i*2+1]; }
                    break;
                }
                case CU32: {
                    const uint32_t* in = (const uint32_t*)item->raw_input_buffer;
                    for (int i = 0; i < item->frames_read; i++) { temp_unnormalized_float_buffer[i] = ((float)in[i*2] - 2147483647.5f) + I * ((float)in[i*2+1] - 2147483647.5f); }
                    break;
                }
                default:
                    handle_fatal_thread_error("Processor thread: Unhandled integer format for pre-processing.", resources);
                    queue_enqueue(resources->free_pool_q, item);
                    continue;
            }

            // Step 2: Apply DC Block (MUST be before I/Q correction).
            if (config->dc_block.enable) {
                pthread_mutex_lock(&resources->dsp_mutex);
                dc_block_apply(resources, temp_unnormalized_float_buffer, item->frames_read);
                pthread_mutex_unlock(&resources->dsp_mutex);
            }

            // Step 3: Apply I/Q Correction.
            if (config->iq_correction.enable) {
                pthread_mutex_lock(&resources->dsp_mutex);
                
                // Accumulator logic to handle variable chunk sizes
                int samples_to_process = item->frames_read;
                int offset = 0;
                while (samples_to_process > 0) {
                    int samples_needed = IQ_CORRECTION_FFT_SIZE - resources->iq_correction.samples_in_accum;
                    int samples_to_copy = (samples_to_process < samples_needed) ? samples_to_process : samples_needed;

                    memcpy(resources->iq_correction.optimization_accum_buffer + resources->iq_correction.samples_in_accum,
                           temp_unnormalized_float_buffer + offset,
                           samples_to_copy * sizeof(complex_float_t));

                    resources->iq_correction.samples_in_accum += samples_to_copy;
                    offset += samples_to_copy;
                    samples_to_process -= samples_to_copy;

                    if (resources->iq_correction.samples_in_accum == IQ_CORRECTION_FFT_SIZE) {
                        if (samples_since_last_opt >= IQ_CORRECTION_DEFAULT_PERIOD) {
                            iq_correct_run_optimization(resources, resources->iq_correction.optimization_accum_buffer);
                            samples_since_last_opt = 0;
                        }
                        resources->iq_correction.samples_in_accum = 0;
                    }
                }
                samples_since_last_opt += item->frames_read;

                // Always apply the latest correction to the full data chunk
                iq_correct_apply(resources, temp_unnormalized_float_buffer, item->frames_read);
                
                pthread_mutex_unlock(&resources->dsp_mutex);
            }

            // Step 4: Normalize and Apply Gain.
            float norm_factor = 1.0f;
            switch (resources->input_format) {
                case CS8:  case CU8:  norm_factor = 128.0f;         break;
                case CS16: case CU16: norm_factor = 32768.0f;       break;
                case CS32: case CU32: norm_factor = 2147483648.0f;  break;
                default: break;
            }
            for (int i = 0; i < item->frames_read; i++) {
                item->complex_buffer_scaled[i] = temp_unnormalized_float_buffer[i] / norm_factor * config->gain;
            }
        }

        // --- MAIN DSP STAGE ---
        complex_float_t *input_for_next_stage = item->complex_buffer_scaled;
        if (resources->shifter_nco != NULL && !config->shift_after_resample) {
            pthread_mutex_lock(&resources->dsp_mutex);
            shift_apply(config, resources, item, SHIFT_STAGE_PRE_RESAMPLE);
            pthread_mutex_unlock(&resources->dsp_mutex);
            input_for_next_stage = item->complex_buffer_shifted;
        }

        unsigned int output_frames_this_chunk = 0;
        pthread_mutex_lock(&resources->dsp_mutex);
        if (resources->is_passthrough) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            memcpy(item->complex_buffer_resampled, input_for_next_stage, output_frames_this_chunk * sizeof(complex_float_t));
        } else {
            msresamp_crcf_execute(resources->resampler,
                                  (liquid_float_complex*)input_for_next_stage,
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

        // --- FINAL CONVERSION STAGE ---
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

/**
 * @brief The writer thread's main function.
 *        It dequeues processed data and writes it to the final destination
 *        (file or stdout), and handles progress reporting.
 */
static void* writer_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;
    int loop_count = 0;

    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->output_q);
        if (!item) break; // NULL item signals shutdown
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
