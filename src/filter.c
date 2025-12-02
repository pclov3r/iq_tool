/**
 * @file filter.c
 * @brief Implements the user-defined FIR/FFT filter chain.
 */

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

// --- Create a real-coefficient (_crcf) filter from the complex master taps ---
#define PREPARE_AND_CREATE_CRCF_FILTER(prefix, ...) \
    do { \
        float* final_real_taps = (float*)mem_arena_alloc(arena, master_taps_len * sizeof(float), false); \
        if (!final_real_taps) goto cleanup; \
        for (int i = 0; i < master_taps_len; i++) { \
            final_real_taps[i] = crealf(master_taps[i]); \
        } \
        resources->user_filter_object = (void*)prefix##_crcf_create(final_real_taps, master_taps_len, ##__VA_ARGS__); \
    } while (0)


// --- Static Helper Functions ---

/**
 * @brief Determines if the filter should be applied before or after resampling for efficiency.
 * This is an important optimization. If downsampling, it checks if the filter's
 * passband is within the Nyquist frequency of the *output* rate. If so, it's
 * more efficient to filter after resampling. This function modifies the config.
 * @param config The application configuration.
 * @param resources The application resources.
 * @return true on success, false if the filter configuration is invalid for the output rate.
 */
static bool _configure_filter_stage(AppConfig *config, AppResources *resources) {
    config->dsp.filter.apply_post_resample = false;

    if (config->dsp.filter.count == 0 || config->dsp.no_resample || config->dsp.raw_passthrough) {
        return true;
    }

    double input_rate = (double)resources->source_info.samplerate;
    double output_rate = config->output_rate.target_rate;

    // This optimization is only relevant if we are downsampling.
    if (output_rate < input_rate) {
        float max_filter_freq_hz = 0.0f;

        // Find the highest frequency required by any filter in the chain.
        for (int i = 0; i < config->dsp.filter.count; i++) {
            const FilterRequest* req = &config->dsp.filter.requests[i];
            float current_max = 0.0f;
            switch (req->type) {
                case FILTER_TYPE_LOWPASS:
                case FILTER_TYPE_HIGHPASS:
                    current_max = fabsf(req->freq1_hz);
                    break;
                case FILTER_TYPE_PASSBAND:
                case FILTER_TYPE_STOPBAND:
                    current_max = fabsf(req->freq1_hz) + (req->freq2_hz / 2.0f);
                    break;
                default:
                    break;
            }
            if (current_max > max_filter_freq_hz) {
                max_filter_freq_hz = current_max;
            }
        }

        double output_nyquist = output_rate / 2.0;

        if (max_filter_freq_hz > output_nyquist) {
            log_fatal("Filter configuration is incompatible with the output sample rate.");
            log_error("The specified filter chain extends to %.0f Hz, but the output rate of %.0f Hz can only support frequencies up to %.0f Hz.",
                      max_filter_freq_hz, output_rate, output_nyquist);
            return false;
        } else {
            // It's safe and more efficient to filter after resampling.
            log_debug("Filter will be applied efficiently after resampling to avoid excessive CPU usage.");
            config->dsp.filter.apply_post_resample = true;
        }
    }
    return true;
}

static inline void _invert_filter_spectrum(float* taps, unsigned int len) {
    for (unsigned int k = 0; k < len; k++) {
        taps[k] = -taps[k];
    }
    taps[(len - 1) / 2] += 1.0f;
}

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

    resources->user_filter_object = NULL;
    resources->user_filter_type_actual = FILTER_IMPL_NONE;
    resources->user_filter_block_size = 0;

    if (config->dsp.filter.count == 0) {
        return true;
    }

    // First, determine the optimal stage for the filter (pre/post resample).
    if (!_configure_filter_stage(config, resources)) {
        goto cleanup;
    }

    int master_taps_len = 1;
    master_taps = (liquid_float_complex*)mem_arena_alloc(arena, sizeof(liquid_float_complex), false);
    if (!master_taps) goto cleanup;
    master_taps[0] = 1.0f + 0.0f * I;

    double sample_rate_for_design = config->dsp.filter.apply_post_resample
                                      ? config->output_rate.target_rate
                                      : (double)resources->source_info.samplerate;

    bool is_final_filter_complex = false;
    bool normalize_by_peak = false;

    log_info("Designing filter coefficients (this may be slow for large filters)...");

    for (int i = 0; i < config->dsp.filter.count; ++i) {
        FilterRequest adjusted_req = config->dsp.filter.requests[i];
        const FilterRequest* req = &adjusted_req;

        if (req->type != FILTER_TYPE_LOWPASS) {
            normalize_by_peak = true;
        }

        unsigned int current_taps_len;
        float attenuation_db = (config->dsp.filter.args.attenuation > 0.0f) ? config->dsp.filter.args.attenuation : RESAMPLER_QUALITY_ATTENUATION_DB;

        if (config->dsp.filter.args.taps > 0) {
            current_taps_len = (unsigned int)config->dsp.filter.args.taps;
        } else {
            float transition_width_hz;
            if (config->dsp.filter.args.transition_width > 0.0f) {
                transition_width_hz = config->dsp.filter.args.transition_width;
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
                    _invert_filter_spectrum(real_taps, current_taps_len);
                    break;
                case FILTER_TYPE_PASSBAND:
                    bw = req->freq2_hz / (float)sample_rate_for_design;
                    liquid_firdes_kaiser(current_taps_len, bw / 2.0f, attenuation_db, 0.0f, real_taps);
                    break;
                case FILTER_TYPE_STOPBAND:
                    bw = req->freq2_hz / (float)sample_rate_for_design;
                    liquid_firdes_kaiser(current_taps_len, bw / 2.0f, attenuation_db, 0.0f, real_taps);
                    _invert_filter_spectrum(real_taps, current_taps_len);
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

    for (int i = 0; i < config->dsp.filter.count; ++i) {
        const FilterRequest* req = &config->dsp.filter.requests[i];
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
    if (config->dsp.filter.args.type_str != NULL) {
        final_choice = config->dsp.filter.type_req;
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
        if (config->dsp.filter.args.fft_size > 0) {
            block_size = (unsigned int)config->dsp.filter.args.fft_size / 2;
            log_info("Using user-specified FFT size of %u (block size: %u).", config->dsp.filter.args.fft_size, block_size);
            if (block_size < (unsigned int)master_taps_len - 1) {
                log_fatal("The specified --filter-fft-size of %d is too small for a filter with %d taps.", config->dsp.filter.args.fft_size, master_taps_len);
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
            resources->user_filter_object = (void*)fftfilt_cccf_create(master_taps, master_taps_len, resources->user_filter_block_size);
            resources->user_filter_type_actual = FILTER_IMPL_FFT_ASYMMETRIC;
        } else {
            PREPARE_AND_CREATE_CRCF_FILTER(fftfilt, resources->user_filter_block_size);
            resources->user_filter_type_actual = FILTER_IMPL_FFT_SYMMETRIC;
        }
    } else { 
        log_info("Preparing FIR (time-domain) filter object...");
        if (is_final_filter_complex) {
            resources->user_filter_object = (void*)firfilt_cccf_create(master_taps, master_taps_len);
            resources->user_filter_type_actual = FILTER_IMPL_FIR_ASYMMETRIC;
        } else {
            PREPARE_AND_CREATE_CRCF_FILTER(firfilt);
            resources->user_filter_type_actual = FILTER_IMPL_FIR_SYMMETRIC;
        }
    }

    if (!resources->user_filter_object) {
        log_fatal("Failed to create final combined filter object.");
        goto cleanup;
    }

    // Now that the filter object is created, allocate its dependent resources (e.g., remainder buffer)
    if (resources->user_filter_object &&
       (resources->user_filter_type_actual == FILTER_IMPL_FFT_SYMMETRIC ||
        resources->user_filter_type_actual == FILTER_IMPL_FFT_ASYMMETRIC))
    {
        if (config->dsp.filter.apply_post_resample) {
            resources->post_fft_remainder_buffer = (complex_float_t*)mem_arena_alloc(
                arena,
                resources->user_filter_block_size * sizeof(complex_float_t),
                true
            );
            resources->post_fft_remainder_len = 0;
            if (!resources->post_fft_remainder_buffer) {
                goto cleanup;
            }
        } else {
            resources->pre_fft_remainder_buffer = (complex_float_t*)mem_arena_alloc(
                arena,
                resources->user_filter_block_size * sizeof(complex_float_t),
                true
            );
            resources->pre_fft_remainder_len = 0;
            if (!resources->pre_fft_remainder_buffer) {
                goto cleanup;
            }
        }
    }

    success = true;

cleanup:
    return success;
}

void filter_destroy(AppResources* resources) {
    if (resources->user_filter_object) {
        switch (resources->user_filter_type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
                firfilt_crcf_destroy((firfilt_crcf)resources->user_filter_object);
                break;
            case FILTER_IMPL_FIR_ASYMMETRIC:
                firfilt_cccf_destroy((firfilt_cccf)resources->user_filter_object);
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
                fftfilt_crcf_destroy((fftfilt_crcf)resources->user_filter_object);
                break;
            case FILTER_IMPL_FFT_ASYMMETRIC:
                fftfilt_cccf_destroy((fftfilt_cccf)resources->user_filter_object);
                break;
            default:
                break;
        }
        resources->user_filter_object = NULL;
    }
}

void filter_reset(AppResources* resources) {
    if (resources->user_filter_object) {
        switch (resources->user_filter_type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
                firfilt_crcf_reset((firfilt_crcf)resources->user_filter_object);
                break;
            case FILTER_IMPL_FIR_ASYMMETRIC:
                firfilt_cccf_reset((firfilt_cccf)resources->user_filter_object);
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
                fftfilt_crcf_reset((fftfilt_crcf)resources->user_filter_object);
                break;
            case FILTER_IMPL_FFT_ASYMMETRIC:
                fftfilt_cccf_reset((fftfilt_cccf)resources->user_filter_object);
                break;
            default:
                break;
        }
    }
}

unsigned int filter_apply(AppResources* resources, SampleChunk* item, bool is_post_resample) {
    if (!resources->user_filter_object) {
        return is_post_resample ? item->frames_to_write : item->frames_read;
    }

    unsigned int frames_in = is_post_resample ? item->frames_to_write : item->frames_read;
    if (frames_in == 0) {
        return 0;
    }

    switch (resources->user_filter_type_actual) {
        case FILTER_IMPL_FIR_SYMMETRIC:
        case FILTER_IMPL_FIR_ASYMMETRIC:
            if (resources->user_filter_type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
                firfilt_crcf_execute_block((firfilt_crcf)resources->user_filter_object,
                                           (liquid_float_complex*)item->current_input_buffer,
                                           frames_in,
                                           (liquid_float_complex*)item->current_input_buffer);
            } else {
                firfilt_cccf_execute_block((firfilt_cccf)resources->user_filter_object,
                                           (liquid_float_complex*)item->current_input_buffer,
                                           frames_in,
                                           (liquid_float_complex*)item->current_input_buffer);
            }
            return frames_in;

        case FILTER_IMPL_FFT_SYMMETRIC:
        case FILTER_IMPL_FFT_ASYMMETRIC:
        {
            complex_float_t* remainder_buffer = is_post_resample ? resources->post_fft_remainder_buffer : resources->pre_fft_remainder_buffer;
            unsigned int* remainder_len_ptr = is_post_resample ? &resources->post_fft_remainder_len : &resources->pre_fft_remainder_len;
            
            unsigned int output_frames = _execute_fft_filter_pass(
                resources->user_filter_object,
                resources->user_filter_type_actual,
                item->current_input_buffer,
                frames_in,
                item->current_output_buffer,
                remainder_buffer,
                remainder_len_ptr,
                resources->user_filter_block_size,
                item->current_output_buffer
            );

            return output_frames;
        }

        default:
             return frames_in;
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

    memcpy(scratch_buffer, remainder_buffer, old_remainder_len * sizeof(complex_float_t));
    memmove(scratch_buffer + old_remainder_len, input_buffer, frames_in * sizeof(complex_float_t));

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

    unsigned int new_remainder_len = total_frames_to_process - processed_frames;
    memmove(remainder_buffer, scratch_buffer + processed_frames, new_remainder_len * sizeof(complex_float_t));
    *remainder_len_ptr = new_remainder_len;

    return total_output_frames;
}

int filter_populate_cli_options(struct argparse_option* buffer, struct AppConfig* config) {
    // Local macros for cleaner definitions inside this function
    #define DEFINE_CHAINABLE_FLOAT_OPTION(name, var, help_text) \
        OPT_FLOAT( 0, name,        &config->dsp.filter.args.var[0], help_text, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-2",     &config->dsp.filter.args.var[1], NULL, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-3",     &config->dsp.filter.args.var[2], NULL, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-4",     &config->dsp.filter.args.var[3], NULL, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-5",     &config->dsp.filter.args.var[4], NULL, NULL, 0, 0)

    #define DEFINE_CHAINABLE_STRING_OPTION(name, var, help_text) \
        OPT_STRING(0, name,        &config->dsp.filter.args.var[0], help_text, NULL, 0, 0), \
        OPT_STRING(0, name "-2",     &config->dsp.filter.args.var[1], NULL, NULL, 0, 0), \
        OPT_STRING(0, name "-3",     &config->dsp.filter.args.var[2], NULL, NULL, 0, 0), \
        OPT_STRING(0, name "-4",     &config->dsp.filter.args.var[3], NULL, NULL, 0, 0), \
        OPT_STRING(0, name "-5",     &config->dsp.filter.args.var[4], NULL, NULL, 0, 0)

    struct argparse_option options[] = {
        OPT_GROUP("Filtering Options (Chain up to 5 by combining options or adding suffixes -2, -3, etc. e.g., --lowpass --stopband --lowpass-2 --pass-range --pass-range-2)"),
        DEFINE_CHAINABLE_FLOAT_OPTION("lowpass", lowpass, "Isolate signal at DC. Keeps freqs from -<hz> to +<hz>."),
        DEFINE_CHAINABLE_FLOAT_OPTION("highpass", highpass, "Remove signal at DC. Rejects freqs from -<hz> to +<hz>."),
        DEFINE_CHAINABLE_STRING_OPTION("pass-range", pass_range, "Isolate a specific band. Format: 'start_freq:end_freq'."),
        DEFINE_CHAINABLE_STRING_OPTION("stopband", stopband, "Remove a specific band (notch). Format: 'start_freq:end_freq'."),
        
        OPT_GROUP("Filter Quality Options"),
        OPT_FLOAT(0, "transition-width", &config->dsp.filter.args.transition_width, "Set filter sharpness by transition width in Hz. (Default: Auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-taps", &config->dsp.filter.args.taps, "Set exact filter length. Overrides --transition-width.", NULL, 0, 0),
        OPT_FLOAT(0, "attenuation", &config->dsp.filter.args.attenuation, "Set filter stop-band attenuation in dB. (Default: 60).", NULL, 0, 0),
        
        OPT_GROUP("Filter Implementation Options (Advanced)"),
        OPT_STRING(0, "filter-type", &config->dsp.filter.args.type_str, "Set filter implementation {fir|fft}. (Default: auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-fft-size", &config->dsp.filter.args.fft_size, "Set FFT size for 'fft' filter type. Must be a power of 2.", NULL, 0, 0),
    };

    size_t count = sizeof(options) / sizeof(options[0]);
    memcpy(buffer, options, sizeof(options));
    return (int)count;
}
