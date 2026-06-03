/**
 * @file filter.c
 * @brief Implements the user-defined FIR/FFT filter chain.
 */

#include "filter.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "mem_arena.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <liquid.h>

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
        app->dsp.filter.object = (struct liquid_filter_s*)prefix##_crcf_create(final_real_taps, master_taps_len, ##__VA_ARGS__); \
    } while (0)

// --- Static Helper Functions ---

/**
 * @brief Determines if the filter should be applied before or after resampling for efficiency.
 * This is an important optimization. If downsampling, it checks if the filter's
 * passband is within the Nyquist frequency of the *output* rate. If so, it's
 * more efficient to filter after resampling. This function modifies the config.
 * @param config The application configuration.
 * @param app The application app.
 * @return true on success, false if the filter configuration is invalid for the output rate.
 */
static bool _configure_filter_stage(AppConfig *config, AppContext* app) {
    config->dsp.filter.apply_post_resample = false;

    if (config->dsp.filter.count == 0 || app->dsp.bypass_resampler || config->dsp.raw_passthrough) {
        return true;
    }

    double input_rate = (double)app->module.source_info.sample_rate;
    double output_sample_rate = config->output_sample_rate.rate_hz;

    // This optimization is only relevant if we are downsampling.
    if (output_sample_rate < input_rate) {
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

        double output_nyquist = output_sample_rate / 2.0;

        if (max_filter_freq_hz > output_nyquist) {
            log_error("Filter configuration is incompatible with the output sample rate.");
            log_error("The specified filter chain extends to %.15g Hz, but the output rate of %.15g Hz can only support frequencies up to %.15g Hz.",
                      max_filter_freq_hz, output_sample_rate, output_nyquist);
            return false;
        } else {
            // It's safe and more efficient to filter after resampling.
            log_debug("Filter will be applied efficiently after resampling to avoid excessive CPU usage.");
            config->dsp.filter.apply_post_resample = true;
        }
    }
    return true;
}

static inline void _normalize_filter_dc_gain(float* taps, unsigned int length) {
    float sum = 0.0f;
    for (unsigned int k = 0; k < length; k++) sum += taps[k];
    if (sum != 0.0f) {
        for (unsigned int k = 0; k < length; k++) taps[k] /= sum;
    }
}

static inline void _invert_filter_spectrum(float* taps, unsigned int length) {
    for (unsigned int k = 0; k < length; k++) {
        taps[k] = -taps[k];
    }
    taps[(length - 1) / 2] += 1.0f;
}

static unsigned int
_execute_fft_filter_pass(
    struct liquid_filter_s* filter_object,
    FilterImplementationType filter_type,
    const ComplexFloat* input_buffer,
    unsigned int frames_in,
    ComplexFloat* output_buffer,
    ComplexFloat* remainder_buffer,
    unsigned int* remainder_len_ptr,
    unsigned int block_size,
    ComplexFloat* scratch_buffer,
    bool is_last_chunk
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

static liquid_float_complex* _generate_base_lowpass_taps(
    unsigned int taps_len, float half_bw_norm, float attenuation_db, MemoryArena* arena)
{
    float* real_taps = (float*)mem_arena_alloc(arena, taps_len * sizeof(float), false);
    if (!real_taps) return NULL;
    liquid_firdes_kaiser(taps_len, half_bw_norm, attenuation_db, 0.0f, real_taps);
    _normalize_filter_dc_gain(real_taps, taps_len);

    liquid_float_complex* complex_taps = (liquid_float_complex*)mem_arena_alloc(arena, taps_len * sizeof(liquid_float_complex), false);
    if (!complex_taps) return NULL;
    for (unsigned int k = 0; k < taps_len; k++) {
        complex_taps[k] = real_taps[k] + 0.0f * I;
    }
    return complex_taps;
}

static void _apply_complex_nco_shift(
    liquid_float_complex* taps, unsigned int taps_len, float fc_norm)
{
    nco_crcf shifter = nco_crcf_create(LIQUID_NCO);
    nco_crcf_set_frequency(shifter, 2.0f * M_PI * fc_norm);

    // Critical DSP Fix: Set initial phase so the center tap has 0 phase offset.
    float m_idx = (float)(taps_len - 1) / 2.0f;
    nco_crcf_set_phase(shifter, -(2.0f * M_PI * fc_norm) * m_idx);

    for (unsigned int k = 0; k < taps_len; k++) {
        liquid_float_complex shift_val;
        nco_crcf_cexpf(shifter, &shift_val);
        taps[k] *= shift_val;
        nco_crcf_step(shifter);
    }
    nco_crcf_destroy(shifter);
}

static void _invert_to_highpass_or_notch(liquid_float_complex* taps, unsigned int taps_len)
{
    for (unsigned int k = 0; k < taps_len; k++) {
        taps[k] = -taps[k];
    }
    taps[(taps_len - 1) / 2] += 1.0f + 0.0f * I;
}

static liquid_float_complex* _compound_filter_stages(
    AppConfig* config, double sample_rate, int* master_len, bool* out_is_complex, bool* out_norm_peak, MemoryArena* arena)
{
    int m_len = 1;
    liquid_float_complex* master_taps = (liquid_float_complex*)mem_arena_alloc(arena, sizeof(liquid_float_complex), false);
    if (!master_taps) return NULL;
    master_taps[0] = 1.0f + 0.0f * I;

    *out_is_complex = false;
    *out_norm_peak = false;

    for (int i = 0; i < config->dsp.filter.count; ++i) {
        FilterRequest* req = &config->dsp.filter.requests[i];

        if (req->type != FILTER_TYPE_LOWPASS) {
            *out_norm_peak = true;
        }

        unsigned int current_taps_len;
        float atten = config->dsp.filter.args.attenuation;

        if (config->dsp.filter.args.taps > 0) {
            current_taps_len = (unsigned int)config->dsp.filter.args.taps;
            if (current_taps_len % 2 == 0) current_taps_len++;
        } else {
            float tw_hz = config->dsp.filter.args.transition_width;
            if (tw_hz <= 0.0f) {
                float ref_freq = (req->type == FILTER_TYPE_LOWPASS || req->type == FILTER_TYPE_HIGHPASS) ? req->freq1_hz : req->freq2_hz;
                tw_hz = fabsf(ref_freq) * DEFAULT_FILTER_TRANSITION_FACTOR;
            }
            if (tw_hz < 1.0f) tw_hz = 1.0f;
            current_taps_len = estimate_req_filter_len(tw_hz / (float)sample_rate, atten);
            if (current_taps_len % 2 == 0) current_taps_len++;
            if (current_taps_len < FILTER_MINIMUM_TAPS) current_taps_len = FILTER_MINIMUM_TAPS;

            if (current_taps_len > FILTER_MAXIMUM_AUTO_TAPS) {
                log_warn("Clamping filter taps to %d", FILTER_MAXIMUM_AUTO_TAPS);
                current_taps_len = FILTER_MAXIMUM_AUTO_TAPS;
            }
        }

        bool is_complex = ((req->type == FILTER_TYPE_PASSBAND || req->type == FILTER_TYPE_STOPBAND) && fabsf(req->freq1_hz) > 1e-9f);
        if (is_complex) *out_is_complex = true;

        float bw_norm = 0.0f;
        if (req->type == FILTER_TYPE_LOWPASS || req->type == FILTER_TYPE_HIGHPASS) {
            bw_norm = req->freq1_hz / (float)sample_rate;
        } else {
            bw_norm = (req->freq2_hz / 2.0f) / (float)sample_rate;
        }

        liquid_float_complex* current_taps = _generate_base_lowpass_taps(current_taps_len, bw_norm, atten, arena);
        if (!current_taps) return NULL;

        if (is_complex) {
            _apply_complex_nco_shift(current_taps, current_taps_len, req->freq1_hz / (float)sample_rate);
        }

        if (req->type == FILTER_TYPE_HIGHPASS || req->type == FILTER_TYPE_STOPBAND) {
            _invert_to_highpass_or_notch(current_taps, current_taps_len);
        }

        int new_len;
        liquid_float_complex* new_master = convolve_complex_taps(master_taps, m_len, current_taps, current_taps_len, &new_len, arena);
        if (!new_master) return NULL;

        master_taps = new_master;
        m_len = new_len;
    }

    *master_len = m_len;
    return master_taps;
}

static struct liquid_filter_s* _compile_filter_object(
    AppConfig* config, AppContext* app, liquid_float_complex* master_taps, int master_taps_len, bool is_final_filter_complex, bool normalize_by_peak, MemoryArena* arena)
{
    if (normalize_by_peak || is_final_filter_complex) {
        log_info("Normalizing filter gain (this may be slow for large filters)...");
        float max_mag = 0.0f;
        firfilt_cccf temp_filter = firfilt_cccf_create(master_taps, master_taps_len);
        if (temp_filter) {
            liquid_float_complex H;
            for (int i = 0; i < FILTER_FREQ_RESPONSE_POINTS; i++) {
                float freq = ((float)i / (float)FILTER_FREQ_RESPONSE_POINTS) - 0.5f;
                firfilt_cccf_freqresponse(temp_filter, freq, &H);
                float mag = cabsf(H);
                if (mag > max_mag) max_mag = mag;
            }
            firfilt_cccf_destroy(temp_filter);
        }
        if (max_mag > FILTER_GAIN_ZERO_THRESHOLD) {
            for (int i = 0; i < master_taps_len; i++) master_taps[i] /= max_mag;
        }
    } else {
        double gain_correction = 0.0;
        for (int i = 0; i < master_taps_len; i++) gain_correction += crealf(master_taps[i]);
        if (fabs(gain_correction) > FILTER_GAIN_ZERO_THRESHOLD) {
            for (int i = 0; i < master_taps_len; i++) master_taps[i] /= (float)gain_correction;
        }
    }

    FilterTypeRequest final_choice;
    if (config->dsp.filter.args.type_str != NULL) {
        final_choice = config->dsp.filter.type_req;
    } else {
        final_choice = is_final_filter_complex ? FILTER_TYPE_FFT : FILTER_TYPE_FIR;
        log_info(is_final_filter_complex ? "Automatically choosing efficient FFT method by default." : "Symmetric filter detected. Using default low-latency FIR method.");
    }

    if (final_choice == FILTER_TYPE_FFT) {
        log_info("Preparing FFT-based filter object (this may take a moment)...");
        unsigned int block_size;
        if (config->dsp.filter.args.fft_size > 0) {
            block_size = (unsigned int)config->dsp.filter.args.fft_size / 2;
            if (block_size < (unsigned int)master_taps_len - 1) return NULL;
        } else {
            block_size = 1;
            while (block_size < (unsigned int)master_taps_len - 1) block_size *= 2;
            if (block_size < (unsigned int)master_taps_len * 2) block_size *= 2;
        }
        app->dsp.filter.block_size = block_size;

        if (is_final_filter_complex) {
            app->dsp.filter.type_actual = FILTER_IMPL_FFT_ASYMMETRIC;
            app->dsp.filter.object = (struct liquid_filter_s*)fftfilt_cccf_create(master_taps, master_taps_len, block_size);
        } else {
            float* real_taps = (float*)mem_arena_alloc(arena, master_taps_len * sizeof(float), false);
            for(int i=0; i<master_taps_len; i++) real_taps[i] = crealf(master_taps[i]);
            app->dsp.filter.type_actual = FILTER_IMPL_FFT_SYMMETRIC;
            app->dsp.filter.object = (struct liquid_filter_s*)fftfilt_crcf_create(real_taps, master_taps_len, block_size);
        }

        size_t scratch_needed = app->pipeline.alloc_size_samples + app->dsp.filter.block_size + 64;
        app->dsp.filter.fft_scratch_buffer = (ComplexFloat*)mem_arena_alloc(arena, scratch_needed * sizeof(ComplexFloat), true);
        if (!app->dsp.filter.fft_scratch_buffer) return NULL;
    } else {
        log_info("Preparing FIR (time-domain) filter object...");
        if (is_final_filter_complex) {
            app->dsp.filter.type_actual = FILTER_IMPL_FIR_ASYMMETRIC;
            app->dsp.filter.object = (struct liquid_filter_s*)firfilt_cccf_create(master_taps, master_taps_len);
        } else {
            float* real_taps = (float*)mem_arena_alloc(arena, master_taps_len * sizeof(float), false);
            for(int i=0; i<master_taps_len; i++) real_taps[i] = crealf(master_taps[i]);
            app->dsp.filter.type_actual = FILTER_IMPL_FIR_SYMMETRIC;
            app->dsp.filter.object = (struct liquid_filter_s*)firfilt_crcf_create(real_taps, master_taps_len);
        }
    }
    return app->dsp.filter.object;
}

bool filter_create(AppConfig* config, AppContext* app, MemoryArena* arena) {
    app->dsp.filter.object = NULL;
    app->dsp.filter.type_actual = FILTER_IMPL_NONE;
    app->dsp.filter.block_size = 0;

    if (config->dsp.filter.count == 0) return true;
    if (!_configure_filter_stage(config, app)) return false;

    double sample_rate = config->dsp.filter.apply_post_resample ? config->output_sample_rate.rate_hz : (double)app->module.source_info.sample_rate;

    int master_len = 0;
    bool is_complex = false, norm_peak = false;
    log_info("Designing filter coefficients (this may be slow for large filters)...");

    liquid_float_complex* master_taps = _compound_filter_stages(config, sample_rate, &master_len, &is_complex, &norm_peak, arena);
    if (!master_taps) return false;

    log_info("Final combined filter requires %d taps.", master_len);
    if (is_complex) log_info("Asymmetric filter detected.");

    if (!_compile_filter_object(config, app, master_taps, master_len, is_complex, norm_peak, arena)) {
        log_fatal("Failed to create final combined filter object.");
        return false;
    }

    if (config->dsp.filter.apply_post_resample) {
        app->dsp.filter.post_fft_remainder_buffer = (ComplexFloat*)mem_arena_alloc(arena, app->dsp.filter.block_size * sizeof(ComplexFloat), true);
        app->dsp.filter.post_fft_remainder_len = 0;
    } else {
        app->dsp.filter.pre_fft_remainder_buffer = (ComplexFloat*)mem_arena_alloc(arena, app->dsp.filter.block_size * sizeof(ComplexFloat), true);
        app->dsp.filter.pre_fft_remainder_len = 0;
    }

    return true;
}

void filter_destroy(AppContext* app) {
    if (app->dsp.filter.object) {
        switch (app->dsp.filter.type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
                firfilt_crcf_destroy((firfilt_crcf)app->dsp.filter.object);
                break;
            case FILTER_IMPL_FIR_ASYMMETRIC:
                firfilt_cccf_destroy((firfilt_cccf)app->dsp.filter.object);
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
                fftfilt_crcf_destroy((fftfilt_crcf)app->dsp.filter.object);
                break;
            case FILTER_IMPL_FFT_ASYMMETRIC:
                fftfilt_cccf_destroy((fftfilt_cccf)app->dsp.filter.object);
                break;
            default:
                break;
        }
        app->dsp.filter.object = NULL;
    }
}

void filter_reset(DspContext* dsp) {
    if (dsp->filter.object) {
        switch (dsp->filter.type_actual) {
            case FILTER_IMPL_FIR_SYMMETRIC:
                firfilt_crcf_reset((firfilt_crcf)dsp->filter.object);
                break;
            case FILTER_IMPL_FIR_ASYMMETRIC:
                firfilt_cccf_reset((firfilt_cccf)dsp->filter.object);
                break;
            case FILTER_IMPL_FFT_SYMMETRIC:
                fftfilt_crcf_reset((fftfilt_crcf)dsp->filter.object);
                break;
            case FILTER_IMPL_FFT_ASYMMETRIC:
                fftfilt_cccf_reset((fftfilt_cccf)dsp->filter.object);
                break;
            default:
                break;
        }
    }
}

unsigned int filter_apply(DspContext* dsp, SampleChunk* item, bool is_post_resample) {
    if (!dsp->filter.object) {
        return is_post_resample ? item->frames_to_write : item->frames_read;
    }

    unsigned int frames_in = is_post_resample ? item->frames_to_write : item->frames_read;
    ComplexFloat* target_buffer = is_post_resample ? item->post_resample_buffer : item->pre_resample_buffer;
    if (frames_in == 0) {
        return 0;
    }

    switch (dsp->filter.type_actual) {
        case FILTER_IMPL_FIR_SYMMETRIC:
        case FILTER_IMPL_FIR_ASYMMETRIC:
            if (dsp->filter.type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
                firfilt_crcf_execute_block((firfilt_crcf)dsp->filter.object,
                                           (liquid_float_complex*)target_buffer,
                                           frames_in,
                                           (liquid_float_complex*)target_buffer);
            } else {
                firfilt_cccf_execute_block((firfilt_cccf)dsp->filter.object,
                                           (liquid_float_complex*)target_buffer,
                                           frames_in,
                                           (liquid_float_complex*)target_buffer);
            }
            return frames_in;

        case FILTER_IMPL_FFT_SYMMETRIC:
        case FILTER_IMPL_FFT_ASYMMETRIC:
        {
            ComplexFloat* remainder_buffer = is_post_resample ? dsp->filter.post_fft_remainder_buffer : dsp->filter.pre_fft_remainder_buffer;
            unsigned int* remainder_len_ptr = is_post_resample ? &dsp->filter.post_fft_remainder_len : &dsp->filter.pre_fft_remainder_len;

            unsigned int output_frames = _execute_fft_filter_pass(
                dsp->filter.object,
                dsp->filter.type_actual,
                target_buffer,
                frames_in,
                target_buffer,
                remainder_buffer,
                remainder_len_ptr,
                dsp->filter.block_size,
                dsp->filter.fft_scratch_buffer,
                item->is_last_chunk
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
    struct liquid_filter_s* filter_object,
    FilterImplementationType filter_type,
    const ComplexFloat* input_buffer,
    unsigned int frames_in,
    ComplexFloat* output_buffer,
    ComplexFloat* remainder_buffer,
    unsigned int* remainder_len_ptr,
    unsigned int block_size,
    ComplexFloat* scratch_buffer,
    bool is_last_chunk
) {
    unsigned int old_remainder_len = *remainder_len_ptr;
    unsigned int total_frames_to_process = old_remainder_len + frames_in;

    memcpy(scratch_buffer, remainder_buffer, old_remainder_len * sizeof(ComplexFloat));
    memcpy(scratch_buffer + old_remainder_len, input_buffer, frames_in * sizeof(ComplexFloat));

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

    // EOF LOGIC: Flush the remaining tail of the file
    if (is_last_chunk && (total_frames_to_process - processed_frames > 0)) {
        unsigned int final_tail_len = total_frames_to_process - processed_frames;

        // Zero-pad the remaining samples up to block_size
        memset(scratch_buffer + processed_frames + final_tail_len, 0,
               (block_size - final_tail_len) * sizeof(ComplexFloat));

        // Force one final FFT execution
        if (filter_type == FILTER_IMPL_FFT_SYMMETRIC) {
            fftfilt_crcf_execute((fftfilt_crcf)filter_object, (liquid_float_complex*)(scratch_buffer + processed_frames), (liquid_float_complex*)(output_buffer + total_output_frames));
        } else {
            fftfilt_cccf_execute((fftfilt_cccf)filter_object, (liquid_float_complex*)(scratch_buffer + processed_frames), (liquid_float_complex*)(output_buffer + total_output_frames));
        }

        processed_frames += final_tail_len; // Only advance by the actual data length
        total_output_frames += final_tail_len; // Only output the actual data length
    }

    unsigned int new_remainder_len = total_frames_to_process - processed_frames;
    if (new_remainder_len > 0) {
        memmove(remainder_buffer, scratch_buffer + processed_frames, new_remainder_len * sizeof(ComplexFloat));
    }
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
        OPT_FLOAT(0, "attenuation", &config->dsp.filter.args.attenuation, "Set global filter & resampler stop-band attenuation in dB. (Default: Auto).", NULL, 0, 0),

        OPT_GROUP("Filter Implementation Options"),
        OPT_STRING(0, "filter-type", &config->dsp.filter.args.type_str, "Set filter implementation {fir|fft}. (Default: auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-fft-size", &config->dsp.filter.args.fft_size, "Set FFT size for 'fft' filter type. Must be a power of 2.", NULL, 0, 0),
    };

    size_t count = sizeof(options) / sizeof(options[0]);
    memcpy(buffer, options, sizeof(options));
    return (int)count;
}
