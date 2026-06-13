/**
 * @file output_am.c
 * @brief AM Audio Demodulation and Output Module
 *
 * This module implements a robust AM demodulator with two modes:
 * 1. Synchronous AM (SYNC) using a Phase-Locked Loop (PLL) for high-fidelity audio.
 * 2. Envelope AM (ENV) using complex magnitude detection.
 *
 * DSP Pipeline:
 * - Block Filtering: Uses liquid-dsp SIMD block execution for high-speed channel filtering.
 * - Demodulation: The PLL uses a smoothed Costas-loop style phase detector.
 *   If the PLL loses lock on a weak signal, it seamlessly falls back to
 *   Magnitude detection to prevent phase-rotation volume fading.
 * - DC Blocking: Carrier extraction is performed post-demodulation using the
 *   Exact DC-Blocking Filter detailed by Richard G. Lyons.
 * - Resampling & Audio Filtering: SIMD block processing for audio cleanup.
 * - Leveling: Audio volume is strictly normalized using an asymmetric Exponential
 *   Moving Average (EMA) envelope tracker to maintain consistent volume at all SNRs.
 */

#include "output_am.h"
#include "constants.h"
#include "audio_output_functions.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "signal_handler.h"
#include "utilities.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <liquid.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Constants ---
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_CHANNELS          2
#define MAX_AUDIO_CUTOFF_HZ     20000.0f

// --- Constraints ---
#define AM_MIN_INPUT_RATE       12000.0
#define AM_MAX_INPUT_RATE       192000.0
#define AM_DEFAULT_INPUT_RATE   48000.0

// --- PLL Tuning (Sync AM) ---
#define AM_PLL_ACQ_BW           100.0f
#define AM_PLL_TRACKING_BW      30.0f
#define AM_PLL_FLYWHEEL_BW      2.0f   // Narrow BW for weak signals
#define AM_PLL_LOCK_TIME        0.5f

// --- Auto-Fallback Limits ---
#define AM_LOCK_TIMEOUT_SEC     3

// --- Internal Structures ---
typedef struct {
    // Pipeline State
    AudioOutputContext* audio_out;

    // DSP Objects (Liquid)
    firfilt_crcf channel_filter;
    nco_crcf pll;
    firfilt_rrrf audio_lpf;
    msresamp_rrrf resamp_out;

    // Exact DC Blocker State
    float dc_x_prev;
    float dc_y_prev;
    float dc_R;

    // Asymmetric Envelope Tracker
    float volume_envelope;

    // Processing State
    float input_samplerate;
    float manual_gain;
    bool sync_mode;
    bool pll_tracking_mode;
    size_t pll_lock_counter;

    // Hysteresis & Signal State
    bool pll_is_locked;
    int unlock_counter;
    int lock_counter;
    float carrier_i_smoothed;

    // Stats Logging
    size_t stat_counter;
    double accum_mag_sq_sum;
    double accum_carrier_freq_sum;
    double accum_i_sum;
    double accum_q_sq_sum;
    size_t accum_pll_count;
    size_t stat_threshold;
    size_t pll_lock_samples;

    // Scratch Buffers for SIMD Block Processing
    liquid_float_complex* filtered_baseband;
    float* mono_buffer;
    float* resamp_buffer;
    float* final_audio_buffer;
    int16_t* interleaved_pcm;
} AmContext;

// --- CLI Config ---
static struct {
    float gain_val;
    float audio_cutoff;
    bool force_envelope;
} s_am_config = {
    .gain_val = 1.0f,
    .audio_cutoff = 5000.0f,
    .force_envelope = 0
};

// --- Module Interface Implementation ---

static bool am_output_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    config->output.sample_format = CF32;

    if (config->baseband_sample_rate.rate_hz == 0.0) {
        config->baseband_sample_rate.rate_hz = AM_DEFAULT_INPUT_RATE;
        config->baseband_sample_rate.provided = true;
    } else {
        double rate = config->baseband_sample_rate.rate_hz;
        if (rate < AM_MIN_INPUT_RATE) {
            log_error("AM: Input rate %.15g Hz is too low for audio (Min: %.15g).", rate, AM_MIN_INPUT_RATE);
            return false;
        }
        if (rate > AM_MAX_INPUT_RATE) {
            log_error("AM: Input rate %.15g Hz is unnecessarily high (Max: %.15g).", rate, AM_MAX_INPUT_RATE);
            return false;
        }
    }
    return true;
}

static bool am_output_initialize(ModuleContext* context) {
    AppContext* res = context->app;

    AmContext* am_context = (AmContext*)mem_arena_alloc(&res->pipeline.setup_arena, sizeof(AmContext), true);
    if (!am_context) return false;
    res->module.output_private_data = am_context;

    am_context->audio_out = audio_output_create(res, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, res->module.source_info.demod_audio_buffer_size);
    if (!am_context->audio_out) return false;

    // 1. Base DSP Configuration
    float input_rate = (float)context->config->baseband_sample_rate.rate_hz;
    if (input_rate < 1.0f) input_rate = 48000.0f;

    am_context->input_samplerate = input_rate;
    am_context->manual_gain = s_am_config.gain_val;
    am_context->sync_mode = !s_am_config.force_envelope;

    if (s_am_config.audio_cutoff > MAX_AUDIO_CUTOFF_HZ) {
        s_am_config.audio_cutoff = MAX_AUDIO_CUTOFF_HZ;
    }

    // Init State Machine
    am_context->pll_is_locked = am_context->sync_mode;
    am_context->unlock_counter = 0;
    am_context->lock_counter = 0;
    am_context->carrier_i_smoothed = 1.0f;

    am_context->stat_counter = 0;
    am_context->accum_mag_sq_sum = 0.0;
    am_context->accum_carrier_freq_sum = 0.0;
    am_context->accum_i_sum = 0.0;
    am_context->accum_q_sq_sum = 0.0;
    am_context->accum_pll_count = 0;
    am_context->stat_threshold = (size_t)(input_rate * CONSOLE_UPDATE_INTERVAL_SEC);
    am_context->pll_lock_samples = (size_t)(input_rate * AM_PLL_LOCK_TIME);

    log_info("AM: Baseband %.15g Hz | Audio Cutoff: %.15g Hz | Mode: %s",
             input_rate, s_am_config.audio_cutoff,
             am_context->sync_mode ? "Synchronous (PLL)" : "Envelope (Magnitude)");

    // 2. A. Baseband Channel Filter
    unsigned int h_len = 63;
    float hc[63];
    float fc_baseband = (s_am_config.audio_cutoff + 1000.0f) / input_rate;
    if (fc_baseband > 0.49f) fc_baseband = 0.49f;
    liquid_firdes_kaiser(h_len, fc_baseband, context->config->dsp.filter.args.attenuation, 0.0f, hc);
    am_context->channel_filter = firfilt_crcf_create(hc, h_len);

    // 2. B. Demodulator (PLL)
    if (am_context->sync_mode) {
        am_context->pll = nco_crcf_create(LIQUID_VCO);
        nco_crcf_pll_set_bandwidth(am_context->pll, AM_PLL_ACQ_BW / input_rate);
    } else {
        am_context->pll = NULL;
    }

    // 2. C. Exact DC Blocker
    float dc_cutoff_hz = 20.0f;
    am_context->dc_R = 1.0f - (2.0f * (float)M_PI * dc_cutoff_hz / input_rate);
    am_context->dc_x_prev = 0.0f;
    am_context->dc_y_prev = 0.0f;

    // 2. D. Audio Volume Leveler
    am_context->volume_envelope = 0.01f;

    // 2. E. Resampler
    float output_resample_ratio = (float)AUDIO_SAMPLE_RATE / input_rate;
    am_context->resamp_out = msresamp_rrrf_create(output_resample_ratio, context->config->dsp.filter.args.attenuation);

    // 2. F. Audio Lowpass Filter
    unsigned int h_length = 63;
    float h[63];
    float fc = s_am_config.audio_cutoff / (float)AUDIO_SAMPLE_RATE;
    if (fc > 0.49f) fc = 0.49f;
    liquid_firdes_kaiser(h_length, fc, context->config->dsp.filter.args.attenuation, 0.0f, h);
    am_context->audio_lpf = firfilt_rrrf_create(h, h_length);

    // 3. Scratch Buffers for Block Processing
    size_t buf_samples = res->pipeline.alloc_size_samples;
    size_t out_buf_samples = (size_t)ceil(buf_samples * output_resample_ratio) + 128;

    am_context->filtered_baseband = mem_arena_alloc(&res->pipeline.setup_arena, buf_samples * sizeof(liquid_float_complex), false);
    am_context->mono_buffer = mem_arena_alloc(&res->pipeline.setup_arena, buf_samples * sizeof(float), false);
    am_context->resamp_buffer = mem_arena_alloc(&res->pipeline.setup_arena, out_buf_samples * sizeof(float), false);
    am_context->final_audio_buffer = mem_arena_alloc(&res->pipeline.setup_arena, out_buf_samples * sizeof(float), false);
    am_context->interleaved_pcm = mem_arena_alloc(&res->pipeline.setup_arena, out_buf_samples * 2 * sizeof(int16_t), false);

    if (!am_context->filtered_baseband || !am_context->mono_buffer || !am_context->resamp_buffer || !am_context->final_audio_buffer || !am_context->interleaved_pcm) return false;

    return true;
}

static void am_output_reset(ModuleContext* context) { (void)context; }

static void am_output_flush(ModuleContext* context) {
    AmContext* am_context = (AmContext*)context->app->module.output_private_data;
    audio_output_clear(am_context->audio_out);
}

static size_t am_output_write_chunk(ModuleContext* context, const void* buffer, size_t input_bytes) {
    AppContext* res = context->app;
    AmContext* am_context = (AmContext*)res->module.output_private_data;

    const float pcm_scale = 32767.0f;



    if (input_bytes == 0) return 0;
    unsigned int num_frames = input_bytes / res->module.output_bytes_per_iq_sample;
    liquid_float_complex* iq_ptr = (liquid_float_complex*)buffer;

    // 1. SIMD Block Processing: Baseband Channel Filter
    // Using execute_block enables high-speed vectorization (AVX/NEON)
    firfilt_crcf_execute_block(am_context->channel_filter, iq_ptr, num_frames, am_context->filtered_baseband);

    // 2. Demodulation Loop (Must be sample-by-sample for PLL feedback loop)
    for (unsigned int i = 0; i < num_frames; i++) {
        liquid_float_complex sample_iq = am_context->filtered_baseband[i];
        float raw_envelope;

        if (am_context->sync_mode) {
            if (!am_context->pll_tracking_mode && am_context->pll_lock_counter++ >= am_context->pll_lock_samples) {
                nco_crcf_pll_set_bandwidth(am_context->pll, AM_PLL_TRACKING_BW / am_context->input_samplerate);
                am_context->pll_tracking_mode = true;
            }

            liquid_float_complex carrier, product;
            nco_crcf_cexpf(am_context->pll, &carrier);
            product = sample_iq * conjf(carrier);

            float prod_i = crealf(product);
            float prod_q = cimagf(product);

            float alpha_i = 100.0f / am_context->input_samplerate;
            am_context->carrier_i_smoothed = (alpha_i * prod_i) + ((1.0f - alpha_i) * am_context->carrier_i_smoothed);

            float phase_error = atan2f(prod_q, prod_i);
            if (am_context->carrier_i_smoothed < 0.0f) {
                phase_error += (phase_error > 0.0f) ? -(float)M_PI : (float)M_PI;
            }

            nco_crcf_pll_step(am_context->pll, phase_error);
            nco_crcf_step(am_context->pll);

            if (am_context->pll_is_locked) {
                raw_envelope = prod_i;
            } else {
                raw_envelope = cabsf(product);
            }

            if ((i & 3) == 0) {
                am_context->accum_carrier_freq_sum += (nco_crcf_get_frequency(am_context->pll) / (2.0f * (float)M_PI)) * am_context->input_samplerate;
                am_context->accum_i_sum += prod_i;
                am_context->accum_q_sq_sum += (prod_q * prod_q);
                am_context->accum_pll_count++;
            }
        } else {
            raw_envelope = cabsf(sample_iq);
        }

        am_context->accum_mag_sq_sum += (cabsf(sample_iq) * cabsf(sample_iq));

        // Exact DC Blocker
        float audio = raw_envelope - am_context->dc_x_prev + (am_context->dc_R * am_context->dc_y_prev);
        am_context->dc_x_prev = raw_envelope;
        am_context->dc_y_prev = audio;

        am_context->mono_buffer[i] = audio;
    }
    am_context->stat_counter += num_frames;

    // --- Stats Logging, SNR dB check, & Hysteresis Logic ---
    if (am_context->stat_counter >= am_context->stat_threshold) {
        float dbfs = 10.0f * log10f((float)(am_context->accum_mag_sq_sum / (double)am_context->stat_counter) + 1e-10f);

        if (am_context->sync_mode && am_context->accum_pll_count > 0) {

            float mean_i = (float)(am_context->accum_i_sum / (double)am_context->accum_pll_count);
            float carrier_power = mean_i * mean_i;
            float noise_power = (float)(am_context->accum_q_sq_sum / (double)am_context->accum_pll_count);

            float snr_db = 0.0f;
            if (noise_power > 1e-12f) {
                snr_db = 10.0f * log10f(carrier_power / noise_power);
            } else {
                snr_db = 99.9f;
            }

            if (dbfs < -70.0f) {
                if (am_context->pll_is_locked) {
                    am_context->pll_is_locked = false;
                    nco_crcf_pll_set_bandwidth(am_context->pll, AM_PLL_FLYWHEEL_BW / am_context->input_samplerate);
                }
                am_context->lock_counter = 0;
                am_context->unlock_counter = 0;
            }
            else if (am_context->pll_is_locked) {
                if (snr_db < 6.0f) {
                    if (++am_context->unlock_counter >= AM_LOCK_TIMEOUT_SEC) {
                        log_warn("AM: PLL failed to lock. Falling back to Envelope detection.");
                        am_context->pll_is_locked = false;
                        am_context->lock_counter = 0;
                        nco_crcf_pll_set_bandwidth(am_context->pll, AM_PLL_FLYWHEEL_BW / am_context->input_samplerate);
                    }
                } else {
                    am_context->unlock_counter = 0;
                }
            }
            else {
                if (snr_db > 10.0f) {
                    if (am_context->lock_counter == 0) {
                        nco_crcf_pll_set_bandwidth(am_context->pll, AM_PLL_ACQ_BW / am_context->input_samplerate);
                    }
                    if (++am_context->lock_counter >= 10) {
                        log_info("AM: PLL locked to carrier.");
                        am_context->pll_is_locked = true;
                        am_context->unlock_counter = 0;
                        nco_crcf_pll_set_bandwidth(am_context->pll, AM_PLL_TRACKING_BW / am_context->input_samplerate);
                    }
                } else {
                    am_context->lock_counter = 0;
                }
            }

            const char* state_tag = am_context->pll_is_locked ? "SYNC PLL LOCKED" : (am_context->lock_counter > 0 ? "ATTEMPTING PLL LOCK" : "ENVELOPE");

            log_info("dBFS: %.1f | SNR: %.1f dB | Offset: %.2f Hz [%s]",
                     dbfs, snr_db, (float)(am_context->accum_carrier_freq_sum / (double)am_context->accum_pll_count), state_tag);

            am_context->accum_carrier_freq_sum=0.0; am_context->accum_i_sum=0.0; am_context->accum_q_sq_sum=0.0; am_context->accum_pll_count=0;
        } else {
            log_info("dBFS: %.1f [ENVELOPE]", dbfs);
        }

        am_context->stat_counter = 0; am_context->accum_mag_sq_sum = 0.0;
    }

    // 3. SIMD Block Processing: Resampler
    unsigned int num_resampled;
    msresamp_rrrf_execute(am_context->resamp_out, am_context->mono_buffer, num_frames, am_context->resamp_buffer, &num_resampled);

    // 4. SIMD Block Processing: Audio Lowpass Filter
    firfilt_rrrf_execute_block(am_context->audio_lpf, am_context->resamp_buffer, num_resampled, am_context->final_audio_buffer);

    // 5. AGC & Volume Leveling (Must be sample-by-sample for EMA state tracking)
    const float attack_alpha = 0.001f;
    const float decay_alpha  = 0.00002f;

    for (unsigned int i = 0; i < num_resampled; i++) {
        float sample = am_context->final_audio_buffer[i];

        float abs_sample = fabsf(sample);
        if (abs_sample > am_context->volume_envelope) {
            am_context->volume_envelope = (attack_alpha * abs_sample) + ((1.0f - attack_alpha) * am_context->volume_envelope);
        } else {
            am_context->volume_envelope = (decay_alpha * abs_sample) + ((1.0f - decay_alpha) * am_context->volume_envelope);
        }

        float safe_volume = fmaxf(am_context->volume_envelope, 0.005f);
        float normalized = (sample / safe_volume) * 0.5f * am_context->manual_gain;

        float clipped = fmaxf(-1.0f, fminf(1.0f, normalized));

        int16_t pcm = (int16_t)(clipped * pcm_scale);
        am_context->interleaved_pcm[2*i] = pcm;
        am_context->interleaved_pcm[2*i + 1] = pcm;
    }

    audio_output_write(am_context->audio_out, am_context->interleaved_pcm, num_resampled * 2 * sizeof(int16_t), res->pipeline_mode);
    return input_bytes;
}

static void am_output_cleanup(ModuleContext* context) {
    AppContext* res = context->app;
    if (!res->module.output_private_data) return;
    AmContext* am_context = (AmContext*)res->module.output_private_data;

    audio_output_destroy(am_context->audio_out);

    if (am_context->channel_filter) firfilt_crcf_destroy(am_context->channel_filter);
    if (am_context->pll) nco_crcf_destroy(am_context->pll);
    if (am_context->audio_lpf) firfilt_rrrf_destroy(am_context->audio_lpf);
    if (am_context->resamp_out) msresamp_rrrf_destroy(am_context->resamp_out);
}

static void am_output_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
    add_summary_item(info, "Output Type", "AM Audio");
    add_summary_item(info, "Mode", "%s", s_am_config.force_envelope ? "Envelope (Mag)" : "Synchronous (PLL)");
    add_summary_item(info, "Audio Sample Rate", "%d Hz", AUDIO_SAMPLE_RATE);
    add_summary_item(info, "Filter Cutoff", "%.15g Hz", s_am_config.audio_cutoff);
}

static const struct argparse_option am_output_cli_options[] = {
    OPT_GROUP("AM Output (am)"),
    OPT_FLOAT(0, "am-gain", &s_am_config.gain_val, "Set audio output gain (linear).", NULL, 0, 0),
    OPT_FLOAT(0, "am-cutoff", &s_am_config.audio_cutoff, "Set audio lowpass filter cutoff in Hz (default: 5000).", NULL, 0, 0),
    OPT_BOOLEAN(0, "am-envelope", &s_am_config.force_envelope, "Disable Synchronous AM (PLL) and use Magnitude Envelope Detection.", NULL, 0, 0),
};

const struct argparse_option* am_output_get_cli_options(int* count) {
    *count = sizeof(am_output_cli_options) / sizeof(am_output_cli_options[0]);
    return am_output_cli_options;
}

static OutputModuleInterface s_am_output_api = {
    .initialize = am_output_initialize,
    .write_chunk = am_output_write_chunk,
    .reset = am_output_reset,
    .flush = am_output_flush,
    .cleanup = am_output_cleanup,
    .get_summary_info = am_output_get_summary_info,
    .validate_options = am_output_validate_options,
    .get_cli_options = am_output_get_cli_options,
};

OutputModuleInterface* output_am_get_module_api(void) {
    return &s_am_output_api;
}
