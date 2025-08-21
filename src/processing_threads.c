#ifdef _WIN32
#include <windows.h>
#endif

#include "processing_threads.h"
#include "types.h"
#include "constants.h"
#include "utils.h"
#include "frequency_shift.h"
#include "signal_handler.h"
#include "log.h"
#include "sample_convert.h"
#include "dc_block.h"
#include "iq_correct.h"
#include "filter.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
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

    complex_float_t* fft_remainder_buffer = NULL;
    unsigned int remainder_len = 0;
    bool is_pre_fft = false;

    if (resources->user_fir_filter_object && !config->apply_user_filter_post_resample) {
        is_pre_fft = (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || 
                      resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC);
        if (is_pre_fft) {
            fft_remainder_buffer = (complex_float_t*)calloc(resources->user_filter_block_size, sizeof(complex_float_t));
            if (!fft_remainder_buffer) {
                handle_fatal_thread_error("Pre-Processor: Failed to allocate FFT remainder buffer.", resources);
                return NULL;
            }
        }
    }

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->raw_to_pre_process_queue)) != NULL) {
        
        if (item->is_last_chunk) {
            if (is_pre_fft && remainder_len > 0) {
                memset(item->complex_pre_resample_data, 0, item->complex_buffer_capacity_samples * sizeof(complex_float_t));
                memcpy(item->complex_pre_resample_data, fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
                
                if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                    fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->complex_pre_resample_data);
                } else {
                    fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->complex_pre_resample_data);
                }
                item->frames_read = resources->user_filter_block_size;
                item->is_last_chunk = false;
                
                queue_enqueue(resources->pre_process_to_resampler_queue, item);

                SampleChunk* final_marker = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (final_marker) {
                    final_marker->is_last_chunk = true;
                    final_marker->frames_read = 0;
                    queue_enqueue(resources->pre_process_to_resampler_queue, final_marker);
                }
            } else {
                queue_enqueue(resources->pre_process_to_resampler_queue, item);
            }
            break;
        }

        if (item->stream_discontinuity_event) {
            shift_reset_nco(resources->pre_resample_nco);
            if (resources->user_fir_filter_object) {
                switch (resources->user_filter_type_actual) {
                    case FILTER_IMPL_FIR_SYMMETRIC: firfilt_crcf_reset((firfilt_crcf)resources->user_fir_filter_object); break;
                    case FILTER_IMPL_FIR_ASYMMETRIC: firfilt_cccf_reset((firfilt_cccf)resources->user_fir_filter_object); break;
                    case FILTER_IMPL_FFT_SYMMETRIC: fftfilt_crcf_reset((fftfilt_crcf)resources->user_fir_filter_object); break;
                    case FILTER_IMPL_FFT_ASYMMETRIC: fftfilt_cccf_reset((fftfilt_cccf)resources->user_fir_filter_object); break;
                    default: break;
                }
            }
            if (is_pre_fft) {
                memset(fft_remainder_buffer, 0, resources->user_filter_block_size * sizeof(complex_float_t));
                remainder_len = 0;
            }
            if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                break;
            }
            continue;
        }

        if (!convert_raw_to_cf32(item->raw_input_data, item->complex_pre_resample_data, item->frames_read, resources->input_format, config->gain)) {
            handle_fatal_thread_error("Pre-Processor: Failed to convert samples.", resources);
            queue_enqueue(resources->free_sample_chunk_queue, item);
            continue;
        }

        if (config->dc_block.enable) {
            dc_block_apply(resources, item->complex_pre_resample_data, item->frames_read);
        }

        if (config->iq_correction.enable) {
            // IQ correction logic...
        }

        if (is_pre_fft) {
            unsigned int block_size = resources->user_filter_block_size;
            unsigned int frames_in = item->frames_read;
            unsigned int total_output_frames = 0;
            unsigned int processed_in_chunk = 0;

            if (remainder_len > 0) {
                unsigned int space_in_remainder = block_size - remainder_len;
                unsigned int to_copy = (frames_in < space_in_remainder) ? frames_in : space_in_remainder;
                memcpy(fft_remainder_buffer + remainder_len, item->complex_pre_resample_data, to_copy * sizeof(complex_float_t));
                processed_in_chunk += to_copy;
                remainder_len += to_copy;

                if (remainder_len == block_size) {
                    if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                        fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, fft_remainder_buffer, item->complex_scratch_data + total_output_frames);
                    } else {
                        fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, fft_remainder_buffer, item->complex_scratch_data + total_output_frames);
                    }
                    total_output_frames += block_size;
                    remainder_len = 0;
                }
            }

            while (frames_in - processed_in_chunk >= block_size) {
                if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                    fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, item->complex_pre_resample_data + processed_in_chunk, item->complex_scratch_data + total_output_frames);
                } else {
                    fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, item->complex_pre_resample_data + processed_in_chunk, item->complex_scratch_data + total_output_frames);
                }
                processed_in_chunk += block_size;
                total_output_frames += block_size;
            }

            if (frames_in - processed_in_chunk > 0) {
                unsigned int leftover = frames_in - processed_in_chunk;
                memcpy(fft_remainder_buffer, item->complex_pre_resample_data + processed_in_chunk, leftover * sizeof(complex_float_t));
                remainder_len = leftover;
            }
            
            memcpy(item->complex_pre_resample_data, item->complex_scratch_data, total_output_frames * sizeof(complex_float_t));
            item->frames_read = total_output_frames;

        } else if (resources->user_fir_filter_object && !config->apply_user_filter_post_resample) {
            firfilt_crcf_execute_block((firfilt_crcf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->frames_read, item->complex_pre_resample_data);
        }

        if (resources->pre_resample_nco) {
            shift_apply(resources->pre_resample_nco, resources->actual_nco_shift_hz, item->complex_pre_resample_data, item->complex_pre_resample_data, item->frames_read);
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

    free(fft_remainder_buffer);
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

    complex_float_t* fft_remainder_buffer = NULL;
    unsigned int remainder_len = 0;
    bool is_post_fft = false;

    if (resources->user_fir_filter_object && config->apply_user_filter_post_resample) {
        is_post_fft = (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || 
                       resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC);
        if (is_post_fft) {
            fft_remainder_buffer = (complex_float_t*)calloc(resources->user_filter_block_size, sizeof(complex_float_t));
            if (!fft_remainder_buffer) {
                handle_fatal_thread_error("Post-Processor: Failed to allocate FFT remainder buffer.", resources);
                return NULL;
            }
        }
    }

    SampleChunk* item;
    while ((item = (SampleChunk*)queue_dequeue(resources->resampler_to_post_process_queue)) != NULL) {
        
        if (item->is_last_chunk) {
            if (is_post_fft && remainder_len > 0) {
                memset(item->complex_resampled_data, 0, item->complex_buffer_capacity_samples * sizeof(complex_float_t));
                memcpy(item->complex_resampled_data, fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
                
                if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                    fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, item->complex_resampled_data, item->complex_resampled_data);
                } else {
                    fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, item->complex_resampled_data, item->complex_resampled_data);
                }
                item->frames_to_write = resources->user_filter_block_size;
                item->is_last_chunk = false;

                complex_float_t* data_in = item->complex_resampled_data;
                if (resources->post_resample_nco) {
                    shift_apply(resources->post_resample_nco, resources->actual_nco_shift_hz, data_in, item->complex_scratch_data, item->frames_to_write);
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
                
                SampleChunk* final_marker = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                if (final_marker) {
                    final_marker->is_last_chunk = true;
                    final_marker->frames_read = 0;
                    item = final_marker;
                } else {
                    break;
                }
            }

            if (config->output_to_stdout) {
                queue_enqueue(resources->stdout_queue, item);
            } else {
                file_write_buffer_signal_end_of_stream(resources->file_write_buffer);
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            break;
        }

        if (item->stream_discontinuity_event) {
            shift_reset_nco(resources->post_resample_nco);
            if (is_post_fft) {
                memset(fft_remainder_buffer, 0, resources->user_filter_block_size * sizeof(complex_float_t));
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
            unsigned int block_size = resources->user_filter_block_size;
            unsigned int frames_in = item->frames_to_write;
            unsigned int total_output_frames = 0;
            unsigned int processed_in_chunk = 0;

            if (remainder_len > 0) {
                unsigned int space_in_remainder = block_size - remainder_len;
                unsigned int to_copy = (frames_in < space_in_remainder) ? frames_in : space_in_remainder;
                memcpy(fft_remainder_buffer + remainder_len, item->complex_resampled_data, to_copy * sizeof(complex_float_t));
                processed_in_chunk += to_copy;
                remainder_len += to_copy;

                if (remainder_len == block_size) {
                    if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                        fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, fft_remainder_buffer, item->complex_scratch_data + total_output_frames);
                    } else {
                        fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, fft_remainder_buffer, item->complex_scratch_data + total_output_frames);
                    }
                    total_output_frames += block_size;
                    remainder_len = 0;
                }
            }

            while (frames_in - processed_in_chunk >= block_size) {
                if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                    fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, item->complex_resampled_data + processed_in_chunk, item->complex_scratch_data + total_output_frames);
                } else {
                    fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, item->complex_resampled_data + processed_in_chunk, item->complex_scratch_data + total_output_frames);
                }
                processed_in_chunk += block_size;
                total_output_frames += block_size;
            }

            if (frames_in - processed_in_chunk > 0) {
                unsigned int leftover = frames_in - processed_in_chunk;
                memcpy(fft_remainder_buffer, item->complex_resampled_data + processed_in_chunk, leftover * sizeof(complex_float_t));
                remainder_len = leftover;
            }
            
            memcpy(item->complex_resampled_data, item->complex_scratch_data, total_output_frames * sizeof(complex_float_t));
            item->frames_to_write = total_output_frames;
        }

        if (item->frames_to_write > 0) {
            complex_float_t* data_in = item->complex_resampled_data;
            complex_float_t* data_out = item->complex_scratch_data;

            if (resources->user_fir_filter_object && config->apply_user_filter_post_resample && !is_post_fft) {
                if (resources->user_filter_type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
                    firfilt_crcf_execute_block((firfilt_crcf)resources->user_fir_filter_object, data_in, item->frames_to_write, data_out);
                } else {
                    firfilt_cccf_execute_block((firfilt_cccf)resources->user_fir_filter_object, data_in, item->frames_to_write, data_out);
                }
                data_in = data_out;
                data_out = item->complex_resampled_data;
            }

            if (resources->post_resample_nco) {
                shift_apply(resources->post_resample_nco, resources->actual_nco_shift_hz, data_in, data_out, item->frames_to_write);
                data_in = data_out;
            }

            if (!convert_cf32_to_block(data_in, item->final_output_data, item->frames_to_write, config->output_format)) {
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

    free(fft_remainder_buffer);
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
