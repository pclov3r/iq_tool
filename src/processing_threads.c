#include "processing_threads.h"
#include "pipeline_context.h"
#include "constants.h"
#include "app_context.h"
#include "utils.h"
#include "frequency_shift.h"
#include "signal_handler.h"
#include "log.h"
#include "sample_convert.h"
#include "dc_block.h"
#include "iq_correct.h"
#include "filter.h"
#include "queue.h"
#include "memory_arena.h"
#include "file_write_buffer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

/**
 * @brief Executes one pass of a block-based FFT filter in a stateful, stream-oriented manner.
 *
 * This function is non-destructive to its input buffer. It uses a temporary
 * working buffer to combine leftover samples from the previous call with new
 * incoming samples, creating a single contiguous stream for processing.
 *
 * @param filter_object         The liquid-dsp fftfilt_crcf or fftfilt_cccf object.
 * @param filter_type           The type of the filter, to select the correct execute function.
 * @param input_buffer          A pointer to the incoming sample data. This buffer is NOT modified.
 * @param frames_in             The number of valid frames in the input_buffer.
 * @param output_buffer         A buffer to store the filtered output blocks. Must be large enough.
 * @param remainder_buffer      The buffer for storing leftover samples between calls.
 * @param remainder_len_ptr     A pointer to the variable holding the current number of samples
 *                              in the remainder buffer. This value is read and updated.
 * @param block_size            The fixed block size the FFT filter operates on.
 * @param scratch_buffer        A large temporary buffer for internal processing. Must be large
 *                              enough to hold (remainder + frames_in) samples.
 * @return The total number of valid output frames written to the output_buffer.
 */
static unsigned int
_execute_fft_filter_pass(
    void* filter_object,
    FilterImplementationType filter_type,
    const complex_float_t* input_buffer,
    unsigned int frames_in,
    complex_float_t* output_buffer,
    complex_float_t* remainder_buffer,
    unsigned int* remainder_len_ptr,
    unsigned int block_size,
    complex_float_t* scratch_buffer
) {
    unsigned int old_remainder_len = *remainder_len_ptr;
    unsigned int total_frames_to_process = old_remainder_len + frames_in;

    // Stage 1: Assemble a single, contiguous stream in the scratch buffer.
    memcpy(scratch_buffer, remainder_buffer, old_remainder_len * sizeof(complex_float_t));
    memcpy(scratch_buffer + old_remainder_len, input_buffer, frames_in * sizeof(complex_float_t));

    // Stage 2: Process full blocks from the assembled stream.
    unsigned int processed_frames = 0;
    unsigned int total_output_frames = 0;
    while (total_frames_to_process - processed_frames >= block_size) {
        if (filter_type == FILTER_IMPL_FFT_SYMMETRIC) {
            fftfilt_crcf_execute((fftfilt_crcf)filter_object, scratch_buffer + processed_frames, output_buffer + total_output_frames);
        } else {
            fftfilt_cccf_execute((fftfilt_cccf)filter_object, scratch_buffer + processed_frames, output_buffer + total_output_frames);
        }
        processed_frames += block_size;
        total_output_frames += block_size;
    }

    // Stage 3: Save the new remainder for the next call.
    unsigned int new_remainder_len = total_frames_to_process - processed_frames;
    // Use memmove for safety, in case buffers could ever overlap.
    memmove(remainder_buffer, scratch_buffer + processed_frames, new_remainder_len * sizeof(complex_float_t));
    *remainder_len_ptr = new_remainder_len;

    return total_output_frames;
}


void* pre_processor_thread_func(void* arg) {
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        log_warn("Failed to set pre-processor thread priority.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    unsigned int remainder_len = 0;
    bool is_pre_fft = false;

    if (resources->user_fir_filter_object && !config->apply_user_filter_post_resample) {
        is_pre_fft = (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || 
                      resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC);
    }

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->raw_to_pre_process_queue)) != NULL) {
        
        if (item->is_last_chunk) {
            if (is_pre_fft && remainder_len > 0) {
                // Flush the remainder by processing it with zero-padding.
                // BUG FIX: Correctly copy remainder then zero-pad the rest of the block.
                memcpy(item->complex_pre_resample_data, resources->pre_fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
                if (resources->user_filter_block_size > remainder_len) {
                    memset(item->complex_pre_resample_data + remainder_len, 0, (resources->user_filter_block_size - remainder_len) * sizeof(complex_float_t));
                }
                
                if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                    fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->complex_pre_resample_data);
                } else {
                    fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->complex_pre_resample_data);
                }
                item->frames_read = resources->user_filter_block_size;
                item->is_last_chunk = false;
                
                queue_enqueue(resources->pre_process_to_resampler_queue, item);

                // Enqueue a final marker chunk after the flushed data.
                SampleChunk* final_marker = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (final_marker) {
                    final_marker->is_last_chunk = true;
                    final_marker->frames_read = 0;
                    queue_enqueue(resources->pre_process_to_resampler_queue, final_marker);
                }
            } else {
                // No flush needed, just pass the end-of-stream marker.
                queue_enqueue(resources->pre_process_to_resampler_queue, item);
            }
            break;
        }

        if (item->stream_discontinuity_event) {
            // Reset all stateful objects in this thread
            freq_shift_reset_nco(resources->pre_resample_nco);
            dc_block_reset(resources);

            if (resources->user_fir_filter_object && !config->apply_user_filter_post_resample) {
                log_debug("Pre-resample filter reset due to stream discontinuity.");
                filter_reset(resources);
            }
            if (is_pre_fft) {
                memset(resources->pre_fft_remainder_buffer, 0, resources->user_filter_block_size * sizeof(complex_float_t));
                remainder_len = 0;
            }
            if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }
            continue;
        }

        if (!convert_raw_to_cf32(item->raw_input_data, item->complex_pre_resample_data, item->frames_read, item->packet_sample_format, config->gain)) {
            handle_fatal_thread_error("Pre-Processor: Failed to convert samples.", resources);
            queue_enqueue(resources->free_sample_chunk_queue, item);
            continue;
        }

        // STEP 1: Apply the DC Block to the main data stream first.
        if (config->dc_block.enable) {
            dc_block_apply(resources, item->complex_pre_resample_data, item->frames_read);
        }

        // STEP 2: Now that the data is clean, handle I/Q correction.
        if (config->iq_correction.enable) {
            // A. If the chunk is large enough, try to get a free buffer and send a
            //    copy of the clean data to the optimization thread for analysis.
            //    This is non-blocking to ensure the main pipeline never stalls.
            if (item->frames_read >= IQ_CORRECTION_FFT_SIZE) {
                SampleChunk* opt_item = (SampleChunk*)queue_try_dequeue(resources->free_sample_chunk_queue);
                if (opt_item) {
                    // Copy the clean data to the new chunk for the optimizer.
                    memcpy(opt_item->complex_pre_resample_data, item->complex_pre_resample_data, IQ_CORRECTION_FFT_SIZE * sizeof(complex_float_t));
                    // Send it to the optimization thread.
                    queue_enqueue(resources->iq_optimization_data_queue, opt_item);
                }
            }
            
            // B. Apply the most recent correction factors to the main data stream.
            iq_correct_apply(resources, item->complex_pre_resample_data, item->frames_read);
        }

        if (is_pre_fft) {
            unsigned int output_frames = _execute_fft_filter_pass(
                resources->user_fir_filter_object,
                resources->user_filter_type_actual,
                item->complex_pre_resample_data,
                (unsigned int)item->frames_read,
                item->complex_scratch_data,
                resources->pre_fft_remainder_buffer,
                &remainder_len,
                resources->user_filter_block_size,
                item->complex_post_resample_data // Use another buffer as scratch space
            );
            memcpy(item->complex_pre_resample_data, item->complex_scratch_data, output_frames * sizeof(complex_float_t));
            item->frames_read = output_frames;
        } else if (resources->user_fir_filter_object && !config->apply_user_filter_post_resample) {
            // BUG FIX: Handle both symmetric and asymmetric FIR filters, not just symmetric.
            if (resources->user_filter_type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
                firfilt_crcf_execute_block((firfilt_crcf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->frames_read, item->complex_pre_resample_data);
            } else { // FILTER_IMPL_FIR_ASYMMETRIC
                firfilt_cccf_execute_block((firfilt_cccf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->frames_read, item->complex_pre_resample_data);
            }
        }

        if (resources->pre_resample_nco) {
            freq_shift_apply(resources->pre_resample_nco, resources->actual_nco_shift_hz, item->complex_pre_resample_data, item->complex_pre_resample_data, item->frames_read);
        }

        if (item->frames_read > 0) {
            if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
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
    while ((item = (SampleChunk*)queue_dequeue(resources->pre_process_to_resampler_queue)) != NULL) {
        if (item->is_last_chunk) {
            queue_enqueue(resources->resampler_to_post_process_queue, item);
            break;
        }

        if (item->stream_discontinuity_event) {
            if (resources->resampler) {
                msresamp_crcf_reset(resources->resampler);
            }
            if (!queue_enqueue(resources->resampler_to_post_process_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }
            continue;
        }

        unsigned int output_frames_this_chunk = 0;
        if (resources->is_passthrough) {
            output_frames_this_chunk = (unsigned int)item->frames_read;
            memcpy(item->complex_resampled_data, item->complex_pre_resample_data, output_frames_this_chunk * sizeof(complex_float_t));
        } else {
            msresamp_crcf_execute(resources->resampler, (liquid_float_complex*)item->complex_pre_resample_data, (unsigned int)item->frames_read, (liquid_float_complex*)item->complex_resampled_data, &output_frames_this_chunk);
        }
        item->frames_to_write = output_frames_this_chunk;

        if (!queue_enqueue(resources->resampler_to_post_process_queue, item)) {
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

    unsigned int remainder_len = 0;
    bool is_post_fft = false;

    if (resources->user_fir_filter_object && config->apply_user_filter_post_resample) {
        is_post_fft = (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || 
                       resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC);
    }

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->resampler_to_post_process_queue)) != NULL) {
        
        if (item->is_last_chunk) {
            if (is_post_fft && remainder_len > 0) {
                // Flush the remainder by processing it with zero-padding.
                // BUG FIX: Correctly copy remainder then zero-pad the rest of the block.
                memcpy(item->complex_resampled_data, resources->post_fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
                if (resources->user_filter_block_size > remainder_len) {
                    memset(item->complex_resampled_data + remainder_len, 0, (resources->user_filter_block_size - remainder_len) * sizeof(complex_float_t));
                }
                
                if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                    fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, item->complex_resampled_data, item->complex_resampled_data);
                } else {
                    fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, item->complex_resampled_data, item->complex_resampled_data);
                }
                item->frames_to_write = resources->user_filter_block_size;
                item->is_last_chunk = false;

                complex_float_t* data_in = item->complex_resampled_data;
                if (resources->post_resample_nco) {
                    freq_shift_apply(resources->post_resample_nco, resources->actual_nco_shift_hz, data_in, item->complex_scratch_data, item->frames_to_write);
                    data_in = item->complex_scratch_data;
                }
                if (!convert_cf32_to_block(data_in, item->final_output_data, item->frames_to_write, config->output_format)) {
                    handle_fatal_thread_error("Post-Processor: Failed to convert final flushed samples.", resources);
                } else {
                    if (config->output_to_stdout) {
                        queue_enqueue(resources->stdout_queue, item);
                    } else {
                        size_t bytes_to_write = item->frames_to_write * resources->output_bytes_per_sample_pair;
                        file_write_buffer_write(resources->file_write_buffer, item->final_output_data, bytes_to_write);
                        queue_enqueue(resources->free_sample_chunk_queue, item);
                    }
                }
                
                // Enqueue a final marker chunk after the flushed data.
                SampleChunk* final_marker = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (final_marker) {
                    final_marker->is_last_chunk = true;
                    final_marker->frames_read = 0;
                    item = final_marker;
                } else {
                    break; // Should not happen if queues are sized correctly
                }
            }

            // Pass the final end-of-stream marker to the writer.
            if (config->output_to_stdout) {
                queue_enqueue(resources->stdout_queue, item);
            } else {
                file_write_buffer_signal_end_of_stream(resources->file_write_buffer);
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            break;
        }

        if (item->stream_discontinuity_event) {
            freq_shift_reset_nco(resources->post_resample_nco);
            if (resources->user_fir_filter_object && config->apply_user_filter_post_resample) {
                log_debug("Post-resample filter reset due to stream discontinuity.");
                filter_reset(resources);
            }
            if (is_post_fft) {
                memset(resources->post_fft_remainder_buffer, 0, resources->user_filter_block_size * sizeof(complex_float_t));
                remainder_len = 0;
            }
            if (config->output_to_stdout) {
                if (!queue_enqueue(resources->stdout_queue, item)) {
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                    break;
                }
            } else {
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            continue;
        }

        if (is_post_fft) {
            unsigned int output_frames = _execute_fft_filter_pass(
                resources->user_fir_filter_object,
                resources->user_filter_type_actual,
                item->complex_resampled_data,
                item->frames_to_write,
                item->complex_scratch_data,
                resources->post_fft_remainder_buffer,
                &remainder_len,
                resources->user_filter_block_size,
                item->complex_post_resample_data // Use another buffer as scratch space
            );
            memcpy(item->complex_resampled_data, item->complex_scratch_data, output_frames * sizeof(complex_float_t));
            item->frames_to_write = output_frames;
        }

        if (item->frames_to_write > 0) {
            // --- Buffer Management for Post-Processing ---
            // To maximize performance, we use a "ping-pong" buffer strategy with pointer swapping.
            // This avoids large, slow memory copies between processing stages (e.g., filter, NCO).
            //
            // - 'current_data_ptr' always points to the buffer holding the latest valid data.
            // - 'workspace_ptr' is used as the destination for the next processing stage.
            // - After each stage, the pointers are swapped, making the output of one stage the
            //   input for the next, all without a single memcpy.
            complex_float_t* current_data_ptr = item->complex_resampled_data;
            complex_float_t* workspace_ptr = item->complex_scratch_data;

            bool is_fir_filter_active = resources->user_fir_filter_object && 
                                        config->apply_user_filter_post_resample && 
                                        !is_post_fft;

            if (is_fir_filter_active) {
                // Input: current_data_ptr, Output: workspace_ptr
                if (resources->user_filter_type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
                    firfilt_crcf_execute_block((firfilt_crcf)resources->user_fir_filter_object, current_data_ptr, item->frames_to_write, workspace_ptr);
                } else {
                    firfilt_cccf_execute_block((firfilt_cccf)resources->user_fir_filter_object, current_data_ptr, item->frames_to_write, workspace_ptr);
                }
                // Swap pointers: The valid data is now in workspace_ptr.
                complex_float_t* temp_ptr = current_data_ptr;
                current_data_ptr = workspace_ptr;
                workspace_ptr = temp_ptr;
            }

            if (resources->post_resample_nco) {
                // Input: current_data_ptr, Output: workspace_ptr
                freq_shift_apply(resources->post_resample_nco, resources->actual_nco_shift_hz, current_data_ptr, workspace_ptr, item->frames_to_write);
                // Swap pointers: The valid data is now in workspace_ptr.
                complex_float_t* temp_ptr = current_data_ptr;
                current_data_ptr = workspace_ptr;
                workspace_ptr = temp_ptr;
            }

            // At this point, 'current_data_ptr' holds the final processed data for this chunk.
            if (!convert_cf32_to_block(current_data_ptr, item->final_output_data, item->frames_to_write, config->output_format)) {
                handle_fatal_thread_error("Post-Processor: Failed to convert samples.", resources);
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }

            if (config->output_to_stdout) {
                if (!queue_enqueue(resources->stdout_queue, item)) {
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                    break;
                }
            } else {
                size_t bytes_to_write = item->frames_to_write * resources->output_bytes_per_sample_pair;
                if (bytes_to_write > 0) {
                    file_write_buffer_write(resources->file_write_buffer, item->final_output_data, bytes_to_write);
                }
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
