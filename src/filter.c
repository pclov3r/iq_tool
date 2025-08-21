#include "filter.h"
#include "constants.h"
#include "log.h"
#include "config.h"
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

static liquid_float_complex* convolve_complex_taps(
    const liquid_float_complex* h1, int len1,
    const liquid_float_complex* h2, int len2,
    int* out_len)
{
    *out_len = len1 + len2 - 1;
    liquid_float_complex* result = (liquid_float_complex*)calloc(*out_len, sizeof(liquid_float_complex));
    if (!result) {
        log_fatal("Failed to allocate memory for complex convolution result.");
        return NULL;
    }

    for (int i = 0; i < *out_len; i++) {
        for (int j = 0; j < len2; j++) {
            if (i - j >= 0 && i - j < len1) {
                result[i] += h1[i - j] * h2[j];
            }
        }
    }
    return result;
}

bool filter_create(AppConfig* config, AppResources* resources) {
    bool success = false;
    liquid_float_complex* master_taps = NULL;

    resources->user_fir_filter_object = NULL;
    resources->user_filter_type_actual = FILTER_IMPL_NONE;
    resources->user_filter_block_size = 0;

    if (config->num_filter_requests == 0) {
        return true;
    }

    int master_taps_len = 1;
    master_taps = (liquid_float_complex*)malloc(sizeof(liquid_float_complex));
    if (!master_taps) {
        log_fatal("Failed to allocate initial master taps.");
        goto cleanup;
    }
    master_taps[0] = 1.0f + 0.0f * I;

    double sample_rate_for_design = config->apply_user_filter_post_resample
                                      ? config->target_rate
                                      : (double)resources->source_info.samplerate;

    bool is_final_filter_complex = false;
    bool normalize_by_peak = false; // Flag to track which normalization to use

    for (int i = 0; i < config->num_filter_requests; ++i) {
        const FilterRequest* req = &config->filter_requests[i];
        
        if (req->type != FILTER_TYPE_LOWPASS) {
            normalize_by_peak = true;
        }
        
        unsigned int current_taps_len;
        float attenuation_db = (config->attenuation_db_arg > 0.0f) ? config->attenuation_db_arg : STOPBAND_ATTENUATION_DB;

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
            if (current_taps_len < 21) current_taps_len = 21;
        }

        liquid_float_complex* current_taps = (liquid_float_complex*)calloc(current_taps_len, sizeof(liquid_float_complex));
        if (!current_taps) {
            log_fatal("Failed to allocate memory for current filter taps.");
            goto cleanup;
        }

        bool is_current_stage_complex = (req->type == FILTER_TYPE_PASSBAND && fabsf(req->freq1_hz) > 1e-9f);
        if (is_current_stage_complex) {
            is_final_filter_complex = true;
        }

        if (is_current_stage_complex) {
            float* real_taps = (float*)malloc(current_taps_len * sizeof(float));
            if (!real_taps) { log_fatal("Failed to alloc real taps."); free(current_taps); goto cleanup; }
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
            free(real_taps);
        } else {
            float* real_taps = (float*)malloc(current_taps_len * sizeof(float));
            if (!real_taps) { log_fatal("Failed to alloc real taps."); free(current_taps); goto cleanup; }
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
            free(real_taps);
        }

        int new_master_len;
        liquid_float_complex* new_master_taps = convolve_complex_taps(master_taps, master_taps_len, current_taps, current_taps_len, &new_master_len);

        free(master_taps);
        free(current_taps);
        if (!new_master_taps) goto cleanup;

        master_taps = new_master_taps;
        master_taps_len = new_master_len;
    }

    log_info("Final combined filter requires %d taps.", master_taps_len);

    if (normalize_by_peak || is_final_filter_complex) {
        float max_mag = 0.0f;
        firfilt_cccf temp_filter = firfilt_cccf_create(master_taps, master_taps_len);
        if (temp_filter) {
            for (int i = 0; i < 2048; i++) {
                liquid_float_complex H;
                float freq = 0.5f * (float)i / 2048.0f;
                firfilt_cccf_freqresponse(temp_filter, freq, &H);
                float mag = cabsf(H);
                if (mag > max_mag) max_mag = mag;
            }
            firfilt_cccf_destroy(temp_filter);
        }
        if (max_mag > 1e-9) {
            log_debug("Normalizing filter taps by peak gain factor of %f.", max_mag);
            for (int i = 0; i < master_taps_len; i++) master_taps[i] /= max_mag;
        }
    } else {
        double gain_correction = 0.0;
        for (int i = 0; i < master_taps_len; i++) {
            gain_correction += crealf(master_taps[i]);
        }
        if (fabs(gain_correction) > 1e-9) {
            log_debug("Normalizing filter taps by DC gain factor of %f.", gain_correction);
            for (int i = 0; i < master_taps_len; i++) master_taps[i] /= (float)gain_correction;
        }
    }

    FilterTypeRequest final_choice;
    if (config->filter_type_str_arg != NULL) {
        final_choice = config->filter_type_request;
    } else {
        if (is_final_filter_complex) {
            log_info("Asymmetric filter detected. Automatically choosing efficient FFT method by default.");
            final_choice = FILTER_TYPE_FFT;
        } else {
            log_info("Symmetric filter detected. Using default low-latency FIR method.");
            final_choice = FILTER_TYPE_FIR;
        }
    }

    if (final_choice == FILTER_TYPE_FFT) {
        log_info("Using FFT-based filter implementation.");
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
            // --- FIX --- Do NOT set master_taps to NULL here. It must be freed at the end.
        } else {
            float* final_real_taps = (float*)malloc(master_taps_len * sizeof(float));
            if (!final_real_taps) { log_fatal("Failed to allocate memory for final real taps."); goto cleanup; }
            for(int i=0; i<master_taps_len; i++) {
                final_real_taps[i] = crealf(master_taps[i]);
            }
            resources->user_fir_filter_object = (void*)fftfilt_crcf_create(final_real_taps, master_taps_len, resources->user_filter_block_size);
            resources->user_filter_type_actual = FILTER_IMPL_FFT_SYMMETRIC;
            // --- FIX --- Free the temporary real taps buffer after it has been copied by liquid-dsp.
            free(final_real_taps);
        }
    } else { 
        log_info("Using FIR (time-domain) filter implementation.");
        if (is_final_filter_complex) {
            resources->user_fir_filter_object = (void*)firfilt_cccf_create(master_taps, master_taps_len);
            resources->user_filter_type_actual = FILTER_IMPL_FIR_ASYMMETRIC;
            // --- FIX --- Do NOT set master_taps to NULL here. It must be freed at the end.
        } else {
            float* final_real_taps = (float*)malloc(master_taps_len * sizeof(float));
            if (!final_real_taps) { log_fatal("Failed to allocate memory for final real taps."); goto cleanup; }
            for(int i=0; i<master_taps_len; i++) {
                final_real_taps[i] = crealf(master_taps[i]);
            }
            resources->user_fir_filter_object = (void*)firfilt_crcf_create(final_real_taps, master_taps_len);
            resources->user_filter_type_actual = FILTER_IMPL_FIR_SYMMETRIC;
            // --- FIX --- Free the temporary real taps buffer after it has been copied by liquid-dsp.
            free(final_real_taps);
        }
    }

    if (!resources->user_fir_filter_object) {
        log_fatal("Failed to create final combined filter object.");
        goto cleanup;
    }

    success = true;

cleanup:
    // This will now correctly free the master_taps buffer in all cases.
    free(master_taps);
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
