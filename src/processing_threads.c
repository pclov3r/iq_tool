// src/processing_threads.c

#ifdef _WIN32
#include <windows.h>
#endif

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
#include "filter.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>

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
    DWORD_PTR affinity_mask = ~(1 << 2);
    if (SetThreadAffinityMask(GetCurrentThread(), affinity_mask) == 0) {
        log_warn("Failed to set pre-processor thread affinity mask.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    complex_float_t* fft_remainder_buffer = NULL;
    unsigned int remainder_len = 0;

    bool is_pre_fft = resources->user_fir_filter_object && !config->apply_user_filter_post_resample &&
                      (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC);

    if (is_pre_fft) {
        fft_remainder_buffer = (complex_float_t*)malloc(resources->user_filter_block_size * sizeof(complex_float_t));
        if (!fft_remainder_buffer) {
            handle_fatal_thread_error("Pre-Processor: Failed to allocate FFT remainder buffer.", resources);
        }
    }

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->raw_to_pre_process_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
            shift_reset_nco(resources->pre_resample_nco);
            if (resources->user_fir_filter_object) {
                // CHANGE: Renamed enum values.
                switch (resources->user_filter_type_actual) {
                    case FILTER_IMPL_FIR_SYMMETRIC: firfilt_crcf_reset((firfilt_crcf)resources->user_fir_filter_object); break;
                    case FILTER_IMPL_FIR_ASYMMETRIC: firfilt_cccf_reset((firfilt_cccf)resources->user_fir_filter_object); break;
                    case FILTER_IMPL_FFT_SYMMETRIC: fftfilt_crcf_reset((fftfilt_crcf)resources->user_fir_filter_object); break;
                    case FILTER_IMPL_FFT_ASYMMETRIC: fftfilt_cccf_reset((fftfilt_cccf)resources->user_fir_filter_object); break;
                    default: break;
                }
            }
            remainder_len = 0;
            if (!queue_enqueue(resources->pre_process_to_resampler_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
            continue;
        }

        if (item->is_last_chunk) {
            if (is_pre_fft && remainder_len > 0) {
                complex_float_t* final_block = (complex_float_t*)calloc(resources->user_filter_block_size, sizeof(complex_float_t));
                if (final_block) {
                    memcpy(final_block, fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
                    if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                        fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, final_block, item->complex_pre_resample_data);
                    } else {
                        fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, final_block, item->complex_pre_resample_data);
                    }
                    item->frames_read = resources->user_filter_block_size;
                    item->is_last_chunk = false;
                    queue_enqueue(resources->pre_process_to_resampler_queue, item);
                    free(final_block);

                    item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
                    if (!item) break;
                }
            }
            item->is_last_chunk = true;
            item->frames_read = 0;
            queue_enqueue(resources->pre_process_to_resampler_queue, item);
            break;
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
            unsigned int total_samples = item->frames_read + remainder_len;
            unsigned int num_blocks = total_samples / block_size;
            unsigned int processed_samples = num_blocks * block_size;
            
            complex_float_t* filter_buffer = (complex_float_t*)malloc(total_samples * sizeof(complex_float_t));
            if (!filter_buffer) {
                handle_fatal_thread_error("Pre-Processor: Failed to allocate FFT filter buffer.", resources);
                queue_enqueue(resources->free_sample_chunk_queue, item);
                continue;
            }
            
            memcpy(filter_buffer, fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
            memcpy(filter_buffer + remainder_len, item->complex_pre_resample_data, item->frames_read * sizeof(complex_float_t));

            if (num_blocks > 0) {
                for (unsigned int i = 0; i < num_blocks; i++) {
                    if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                        fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, filter_buffer + (i * block_size), item->complex_pre_resample_data + (i * block_size));
                    } else {
                        fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, filter_buffer + (i * block_size), item->complex_pre_resample_data + (i * block_size));
                    }
                }
            }
            
            remainder_len = total_samples - processed_samples;
            if (remainder_len > 0) {
                memcpy(fft_remainder_buffer, filter_buffer + processed_samples, remainder_len * sizeof(complex_float_t));
            }
            item->frames_read = processed_samples;
            free(filter_buffer);

        } else if (resources->user_fir_filter_object && !config->apply_user_filter_post_resample) {
            // CHANGE: Renamed enum values.
            if (resources->user_filter_type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
                firfilt_crcf_execute_block((firfilt_crcf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->frames_read, item->complex_pre_resample_data);
            } else {
                firfilt_cccf_execute_block((firfilt_cccf)resources->user_fir_filter_object, item->complex_pre_resample_data, item->frames_read, item->complex_pre_resample_data);
            }
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
    return NULL;
}

void* resampler_thread_func(void* arg) {
    // This function is correct and does not need changes.
    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->pre_process_to_resampler_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
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
            msresamp_crcf_execute(resources->resampler, (liquid_float_complex*)item->complex_pre_resample_data, (unsigned int)item->frames_read, (liquid_float_complex*)item->complex_resampled_data, &output_frames_this_chunk);
        }
        item->frames_to_write = output_frames_this_chunk;

        if (!queue_enqueue(resources->resampler_to_post_process_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
            break;
        }
    }
    return NULL;
}

void* post_processor_thread_func(void* arg) {
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
        log_warn("Failed to set post-processor thread priority.");
    }
    DWORD_PTR affinity_mask = ~(1 << 2);
    if (SetThreadAffinityMask(GetCurrentThread(), affinity_mask) == 0) {
        log_warn("Failed to set post-processor thread affinity mask.");
    }
#endif

    PipelineContext* args = (PipelineContext*)arg;
    AppResources* resources = args->resources;
    AppConfig* config = args->config;

    complex_float_t* fft_remainder_buffer = NULL;
    unsigned int remainder_len = 0;

    bool is_post_fft = resources->user_fir_filter_object && config->apply_user_filter_post_resample &&
                       (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC || resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC);

    if (is_post_fft) {
        fft_remainder_buffer = (complex_float_t*)malloc(resources->user_filter_block_size * sizeof(complex_float_t));
        if (!fft_remainder_buffer) {
            handle_fatal_thread_error("Post-Processor: Failed to allocate FFT remainder buffer.", resources);
        }
    }

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(resources->resampler_to_post_process_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
            shift_reset_nco(resources->post_resample_nco);
            remainder_len = 0;
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

        if (item->is_last_chunk) {
            if (is_post_fft && remainder_len > 0) {
                complex_float_t* final_block = (complex_float_t*)calloc(resources->user_filter_block_size, sizeof(complex_float_t));
                if (final_block) {
                    memcpy(final_block, fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
                    if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                        fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, final_block, item->complex_resampled_data);
                    } else {
                        fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, final_block, item->complex_resampled_data);
                    }
                    item->frames_to_write = resources->user_filter_block_size;
                    item->is_last_chunk = false;
                    free(final_block);
                    // Fall through to process this final data chunk.
                }
            } else {
                if (config->output_to_stdout) {
                    queue_enqueue(resources->stdout_queue, item);
                } else {
                    file_write_buffer_signal_end_of_stream(resources->file_write_buffer);
                    queue_enqueue(resources->free_sample_chunk_queue, item);
                }
                break;
            }
        }

        complex_float_t* current_data = item->complex_resampled_data;
        complex_float_t* scratch_buffer = item->complex_post_resample_data;

        if (is_post_fft) {
            unsigned int block_size = resources->user_filter_block_size;
            unsigned int total_samples = item->frames_to_write + remainder_len;
            unsigned int num_blocks = total_samples / block_size;
            unsigned int processed_samples = num_blocks * block_size;

            complex_float_t* filter_buffer = (complex_float_t*)malloc(total_samples * sizeof(complex_float_t));
            if (!filter_buffer) {
                handle_fatal_thread_error("Post-Processor: Failed to allocate FFT filter buffer.", resources);
                queue_enqueue(resources->free_sample_chunk_queue, item);
                continue;
            }
            
            memcpy(filter_buffer, fft_remainder_buffer, remainder_len * sizeof(complex_float_t));
            memcpy(filter_buffer + remainder_len, current_data, item->frames_to_write * sizeof(complex_float_t));

            if (num_blocks > 0) {
                for (unsigned int i = 0; i < num_blocks; i++) {
                    if (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC) {
                        fftfilt_crcf_execute((fftfilt_crcf)resources->user_fir_filter_object, filter_buffer + (i * block_size), scratch_buffer + (i * block_size));
                    } else {
                        fftfilt_cccf_execute((fftfilt_cccf)resources->user_fir_filter_object, filter_buffer + (i * block_size), scratch_buffer + (i * block_size));
                    }
                }
            }
            
            remainder_len = total_samples - processed_samples;
            if (remainder_len > 0) {
                memcpy(fft_remainder_buffer, filter_buffer + processed_samples, remainder_len * sizeof(complex_float_t));
            }
            item->frames_to_write = processed_samples;
            current_data = scratch_buffer;
            free(filter_buffer);

        } else if (resources->user_fir_filter_object && config->apply_user_filter_post_resample) {
            // CHANGE: Renamed enum values.
            if (resources->user_filter_type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
                firfilt_crcf_execute_block((firfilt_crcf)resources->user_fir_filter_object, current_data, item->frames_to_write, scratch_buffer);
            } else {
                firfilt_cccf_execute_block((firfilt_cccf)resources->user_fir_filter_object, current_data, item->frames_to_write, scratch_buffer);
            }
            current_data = scratch_buffer;
        }

        if (resources->post_resample_nco) {
            complex_float_t* nco_output_buffer = (current_data == item->complex_resampled_data) ? scratch_buffer : item->complex_resampled_data;
            shift_apply(resources->post_resample_nco, resources->actual_nco_shift_hz, current_data, nco_output_buffer, item->frames_to_write);
            current_data = nco_output_buffer;
        }

        if (item->frames_to_write > 0) {
            if (!convert_cf32_to_block(current_data, item->final_output_data, item->frames_to_write, config->output_format)) {
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
                    size_t bytes_written = file_write_buffer_write(resources->file_write_buffer, item->final_output_data, bytes_to_write);
                    if (bytes_written < bytes_to_write) {
                        log_warn("I/O buffer overrun! Dropped %zu bytes. System may be overloaded.", bytes_to_write - bytes_written);
                    }
                }
                queue_enqueue(resources->free_sample_chunk_queue, item);
            }
        } else {
            queue_enqueue(resources->free_sample_chunk_queue, item);
        }

        if (item->is_last_chunk) {
            SampleChunk* final_marker = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
            if (final_marker) {
                final_marker->is_last_chunk = true;
                final_marker->frames_read = 0;
                if (config->output_to_stdout) {
                    queue_enqueue(resources->stdout_queue, final_marker);
                } else {
                    file_write_buffer_signal_end_of_stream(resources->file_write_buffer);
                    queue_enqueue(resources->free_sample_chunk_queue, final_marker);
                }
            }
            break;
        }
    }

    free(fft_remainder_buffer);
    return NULL;
}

void* iq_optimization_thread_func(void* arg) {
    // This function is correct and does not need changes.
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
