#include "filter.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "memory_arena.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Forward declaration for static helper function
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
);

static liquid_float_complex* convolve_complex_taps(
    const liquid_float_complex* h1, int len1,
    const liquid_float_complex* h2, int len2,
    int* out_len, MemoryArena* arena)
{
    *out_len = len1 + len2 - 1;
    liquid_float_complex* result = (liquid_float_complex*)mem_arena_alloc(arena, *out_len * sizeof(liquid_float_complex), false);
    if (!result) {
        return NULL;
    }

    memset(result, 0, *out_len * sizeof(liquid_float_complex));

       for (int i = 0; i < *out_len; i++) {
        int j_start = (i >= len1) ? (i - len1 + 1) : 0;
        int j_end   = (i < len2 - 1) ? i : (len2 - 1);

        for (int j = j_start; j <= j_end; j++) {
            result[i] += h1[i - j] * h2[j];
        }
    }
    return result;
}

bool filter_create(AppConfig* config, AppResources* resources, MemoryArena* arena) {
    bool success = false;
    liquid_float_complex* master_taps = NULL;

    resources->user_fir_filter_object = NULL;
    resources->user_filter_type_actual = FILTER_IMPL_NONE;
    resources->user_filter_block_size = 0;

    if (config->num_filter_requests == 0) {
        return true;
    }

    int master_taps_len = 1;
    master_taps = (liquid_float_complex*)mem_arena_alloc(arena, sizeof(liquid_float_complex), false);
    if (!master_taps) goto cleanup;
    master_taps[0] = 1.0f + 0.0f * I;

    double sample_rate_for_design = config->apply_user_filter_post_resample
                                      ? config->target_rate
                                      : (double)resources->source_info.samplerate;

    bool is_final_filter_complex = false;
    bool normalize_by_peak = false;

    log_info("Designing filter coefficients (this may be slow for large filters)...");

    for (int i = 0; i < config->num_filter_requests; ++i) {
        // Create a local copy of the request that we can modify.
        FilterRequest adjusted_req = config->filter_requests[i];

        // If a pre-resample frequency shift is active, we must compensate the
        // filter's center frequency to match the user's intent.
        if (resources->pre_resample_nco) {
            log_debug("Compensating filter design for active frequency shift of %.0f Hz.", resources->nco_shift_hz);
            adjusted_req.freq1_hz -= resources->nco_shift_hz;
        }

        const FilterRequest* req = &adjusted_req;

        if (req->type != FILTER_TYPE_LOWPASS) {
            normalize_by_peak = true;
        }

        unsigned int current_taps_len;
        float attenuation_db = (config->attenuation_db_arg > 0.0f) ? config->attenuation_db_arg : RESAMPLER_QUALITY_ATTENUATION_DB;

        if (config->filter_taps_arg > 0) {
            current_taps_len = (unsigned int)config->filter_taps_arg;
        } else {
            float transition_width_hz;
            if (config->transition_width_hz_arg > 0.0f) {
                transition_width_hz = config->transition_width_hz_arg;
            } else {
                float reference_freq = (req->type == FILTER_TYPE_LOWPASS || req->type == FILTER_TYPE_HIGHPASS) ? req->freq1_hz : req->freq2_hz;
                transition_width_hz = fabsf(reference_freq) * DEFAULT_FILTER_TRANSITION_FACTOR;
            }
            if (transition_width_hz < 1.0f) transition_width_hz = 1.0f;
            float normalized_tw = transition_width_hz / (float)sample_rate_for_design;
            current_taps_len = estimate_req_filter_len(normalized_tw, attenuation_db);
            if (current_taps_len % 2 == 0) current_taps_len++;
            if (current_taps_len < FILTER_MINIMUM_TAPS) current_taps_len = FILTER_MINIMUM_TAPS;
        }

        liquid_float_complex* current_taps = (liquid_float_complex*)mem_arena_alloc(arena, current_taps_len * sizeof(liquid_float_complex), false);
        if (!current_taps) goto cleanup;

        bool is_current_stage_complex = (req->type == FILTER_TYPE_PASSBAND && fabsf(req->freq1_hz) > 1e-9f);
        if (is_current_stage_complex) {
            is_final_filter_complex = true;
        }

        if (is_current_stage_complex) {
            float* real_taps = (float*)mem_arena_alloc(arena, current_taps_len * sizeof(float), false);
            if (!real_taps) goto cleanup;
            float half_bw_norm = (req->freq2_hz / 2.0f) / (float)sample_rate_for_design;
            liquid_firdes_kaiser(current_taps_len, half_bw_norm, attenuation_db, 0.0f, real_taps);
            float fc_norm = req->freq1_hz / (float)sample_rate_for_design;
            nco_crcf shifter = nco_crcf_create(LIQUID_NCO);
            nco_crcf_set_frequency(shifter, 2.0f * M_PI * fc_norm);
            for (unsigned int k = 0; k < current_taps_len; k++) {
                nco_crcf_cexpf(shifter, &current_taps[k]);
                current_taps[k] *= real_taps[k];
                nco_crcf_step(shifter);
            }
            nco_crcf_destroy(shifter);
        } else {
            float* real_taps = (float*)mem_arena_alloc(arena, current_taps_len * sizeof(float), false);
            if (!real_taps) goto cleanup;
            float fc, bw;
            switch (req->type) {
                case FILTER_TYPE_LOWPASS:
                    fc = req->freq1_hz / (float)sample_rate_for_design;
                    liquid_firdes_kaiser(current_taps_len, fc, attenuation_db, 0.0f, real_taps);
                    break;
                case FILTER_TYPE_HIGHPASS:
                    fc = req->freq1_hz / (float)sample_rate_for_design;
                    liquid_firdes_kaiser(current_taps_len, fc, attenuation_db, 0.0f, real_taps);
                    for (unsigned int k = 0; k < current_taps_len; k++) real_taps[k] = -real_taps[k];
                    real_taps[(current_taps_len - 1) / 2] += 1.0f;
                    break;
                case FILTER_TYPE_PASSBAND:
                    bw = req->freq2_hz / (float)sample_rate_for_design;
                    liquid_firdes_kaiser(current_taps_len, bw / 2.0f, attenuation_db, 0.0f, real_taps);
                    break;
                case FILTER_TYPE_STOPBAND:
                    bw = req->freq2_hz / (float)sample_rate_for_design;
                    liquid_firdes_kaiser(current_taps_len, bw / 2.0f, attenuation_db, 0.0f, real_taps);
                    for (unsigned int k = 0; k < current_taps_len; k++) real_taps[k] = -real_taps[k];
                    real_taps[(current_taps_len - 1) / 2] += 1.0f;
                    break;
                default: break;
            }
            for (unsigned int k = 0; k < current_taps_len; k++) {
                current_taps[k] = real_taps[k] + 0.0f * I;
            }
        }

        int new_master_len;
        liquid_float_complex* new_master_taps = convolve_complex_taps(master_taps, master_taps_len, current_taps, current_taps_len, &new_master_len, arena);

        if (!new_master_taps) goto cleanup;

        master_taps = new_master_taps;
        master_taps_len = new_master_len;
    }

    log_info("Final combined filter requires %d taps.", master_taps_len);

    for (int i = 0; i < config->num_filter_requests; ++i) {
        const FilterRequest* req = &config->filter_requests[i];
        if (req->type == FILTER_TYPE_PASSBAND && fabsf(req->freq1_hz) > 1e-9f) {
            is_final_filter_complex = true;
            break;
        }
    }

    if (is_final_filter_complex) {
        log_info("Asymmetric filter detected.");
    }

    if (normalize_by_peak || is_final_filter_complex) {
        log_info("Normalizing filter gain (this may be slow for large filters)...");
        float max_mag = 0.0f;
        firfilt_cccf temp_filter = firfilt_cccf_create(master_taps, master_taps_len);
        if (temp_filter) {
            for (int i = 0; i < FILTER_FREQ_RESPONSE_POINTS; i++) {
                liquid_float_complex H;
                float freq = ((float)i / (float)FILTER_FREQ_RESPONSE_POINTS) - 0.5f;
                firfilt_cccf_freqresponse(temp_filter, freq, &H);
                float mag = cabsf(H);
                if (mag > max_mag) max_mag = mag;
            }
            firfilt_cccf_destroy(temp_filter);
        }
        if (max_mag > FILTER_GAIN_ZERO_THRESHOLD) {
            log_debug("Normalizing filter taps by peak gain factor of %f.", max_mag);
            for (int i = 0; i < master_taps_len; i++) master_taps[i] /= max_mag;
        }
    } else {
        double gain_correction = 0.0;
        for (int i = 0; i < master_taps_len; i++) {
            gain_correction += crealf(master_taps[i]);
        }
        if (fabs(gain_correction) > FILTER_GAIN_ZERO_THRESHOLD) {
            log_debug("Normalizing filter taps by DC gain factor of %f.", gain_correction);
            for (int i = 0; i < master_taps_len; i++) master_taps[i] /= (float)gain_correction;
        }
    }

    FilterTypeRequest final_choice;
    if (config->filter_type_str_arg != NULL) {
        final_choice = config->filter_type_request;
    } else {
        if (is_final_filter_complex) {
            log_info("Automatically choosing efficient FFT method by default.");
            final_choice = FILTER_TYPE_FFT;
        } else {
            log_info("Symmetric filter detected. Using default low-latency FIR method.");
            final_choice = FILTER_TYPE_FIR;
        }
    }

    if (final_choice == FILTER_TYPE_FFT) {
        log_info("Preparing FFT-based filter object (this may take a moment)...");

        unsigned int block_size;
        if (config->filter_fft_size_arg > 0) {
            block_size = (unsigned int)config->filter_fft_size_arg / 2;
            log_info("Using user-specified FFT size of %u (block size: %u).", config->filter_fft_size_arg, block_size);
            if (block_size < (unsigned int)master_taps_len - 1) {
                log_fatal("The specified --filter-fft-size of %d is too small for a filter with %d taps.", config->filter_fft_size_arg, master_taps_len);
                log_error("A block size (_n) of at least %d is required, meaning an FFT size of at least %d.", master_taps_len - 1, (master_taps_len - 1) * 2);
                goto cleanup;
            }
        } else {
            block_size = 1;
            while (block_size < (unsigned int)master_taps_len - 1) {
                block_size *= 2;
            }
            if (block_size < (unsigned int)master_taps_len * 2) {
                 block_size *= 2;
            }
            log_info("Using automatically calculated block size of %u (FFT size: %u) for filter.", block_size, block_size * 2);
        }
        resources->user_filter_block_size = block_size;

        if (is_final_filter_complex) {
            resources->user_fir_filter_object = (void*)fftfilt_cccf_create(master_taps, master_taps_len, resources->user_filter_block_size);
            resources->user_filter_type_actual = FILTER_IMPL_FFT_ASYMMETRIC;
        } else {
            float* final_real_taps = (float*)mem_arena_alloc(arena, master_taps_len * sizeof(float), false);
            if (!final_real_taps) goto cleanup;
            for(int i=0; i<master_taps_len; i++) {
                final_real_taps[i] = crealf(master_taps[i]);
            }
            resources->user_fir_filter_object = (void*)fftfilt_crcf_create(final_real_taps, master_taps_len, resources->user_filter_block_size);
            resources->user_filter_type_actual = FILTER_IMPL_FFT_SYMMETRIC;
        }
    } else { 
        log_info("Preparing FIR (time-domain) filter object...");
        if (is_final_filter_complex) {
            resources->user_fir_filter_object = (void*)firfilt_cccf_create(master_taps, master_taps_len);
            resources->user_filter_type_actual = FILTER_IMPL_FIR_ASYMMETRIC;
        } else {
            float* final_real_taps = (float*)mem_arena_alloc(arena, master_taps_len * sizeof(float), false);
            if (!final_real_taps) goto cleanup;
            for(int i=0; i<master_taps_len; i++) {
                final_real_taps[i] = crealf(master_taps[i]);
            }
            resources->user_fir_filter_object = (void*)firfilt_crcf_create(final_real_taps, master_taps_len);
            resources->user_filter_type_actual = FILTER_IMPL_FIR_SYMMETRIC;
        }
    }

    if (!resources->user_fir_filter_object) {
        log_fatal("Failed to create final combined filter object.");
        goto cleanup;
    }

    success = true;

cleanup:
    return success;
}

void filter_destroy(AppResources* resources) {
    if (resources->user_fir_filter_object) {
        switch (resources->user_filter_type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
                firfilt_crcf_destroy((firfilt_crcf)resources->user_fir_filter_object);
                break;
            case FILTER_IMPL_FIR_ASYMMETRIC:
                firfilt_cccf_destroy((firfilt_cccf)resources->user_fir_filter_object);
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
                fftfilt_crcf_destroy((fftfilt_crcf)resources->user_fir_filter_object);
                break;
            case FILTER_IMPL_FFT_ASYMMETRIC:
                fftfilt_cccf_destroy((fftfilt_cccf)resources->user_fir_filter_object);
                break;
            default:
                break;
        }
        resources->user_fir_filter_object = NULL;
    }
}

void filter_reset(AppResources* resources) {
    if (resources->user_fir_filter_object) {
        switch (resources->user_filter_type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
                firfilt_crcf_reset((firfilt_crcf)resources->user_fir_filter_object);
                break;
            case FILTER_IMPL_FIR_ASYMMETRIC:
                firfilt_cccf_reset((firfilt_cccf)resources->user_fir_filter_object);
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
                fftfilt_crcf_reset((fftfilt_crcf)resources->user_fir_filter_object);
                break;
            case FILTER_IMPL_FFT_ASYMMETRIC:
                fftfilt_cccf_reset((fftfilt_cccf)resources->user_fir_filter_object);
                break;
            default:
                break;
        }
    }
}

unsigned int filter_apply(AppResources* resources, SampleChunk* item, bool is_post_resample) {
    if (!resources->user_fir_filter_object) {
        return is_post_resample ? item->frames_to_write : item->frames_read;
    }

    complex_float_t* data_ptr = is_post_resample ? item->complex_resampled_data : item->complex_pre_resample_data;
    unsigned int frames = is_post_resample ? item->frames_to_write : item->frames_read;

    // This state management for remainders is simplistic and not perfectly thread-safe
    // if the same filter instance were ever used from multiple threads, but it is
    // sufficient for this pipeline where pre- and post-filters are distinct.
    static unsigned int pre_remainder_len = 0;
    static unsigned int post_remainder_len = 0;

    switch (resources->user_filter_type_actual) {
        case FILTER_IMPL_FIR_SYMMETRIC:
            firfilt_crcf_execute_block((firfilt_crcf)resources->user_fir_filter_object, (liquid_float_complex*)data_ptr, frames, (liquid_float_complex*)data_ptr);
            return frames;

        case FILTER_IMPL_FIR_ASYMMETRIC:
            firfilt_cccf_execute_block((firfilt_cccf)resources->user_fir_filter_object, (liquid_float_complex*)data_ptr, frames, (liquid_float_complex*)data_ptr);
            return frames;

        case FILTER_IMPL_FFT_SYMMETRIC:
        case FILTER_IMPL_FFT_ASYMMETRIC:
        {
            complex_float_t* remainder_buffer = is_post_resample ? resources->post_fft_remainder_buffer : resources->pre_fft_remainder_buffer;
            unsigned int* remainder_len_ptr = is_post_resample ? &post_remainder_len : &pre_remainder_len;

            unsigned int output_frames = _execute_fft_filter_pass(
                resources->user_fir_filter_object,
                resources->user_filter_type_actual,
                data_ptr,
                frames,
                item->complex_scratch_data, // Use scratch as the destination
                remainder_buffer,
                remainder_len_ptr,
                resources->user_filter_block_size,
                item->complex_post_resample_data // Use another buffer as internal scratch
            );
            // The result is in the scratch buffer. Copy it back to the primary buffer for the next stage.
            memcpy(data_ptr, item->complex_scratch_data, output_frames * sizeof(complex_float_t));
            return output_frames;
        }

        default:
             return frames;
    }
}

// Actual definition of the helper function
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
            fftfilt_crcf_execute((fftfilt_crcf)filter_object, (liquid_float_complex*)(scratch_buffer + processed_frames), (liquid_float_complex*)(output_buffer + total_output_frames));
        } else {
            fftfilt_cccf_execute((fftfilt_cccf)filter_object, (liquid_float_complex*)(scratch_buffer + processed_frames), (liquid_float_complex*)(output_buffer + total_output_frames));
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
