/**
 * @file output_nfm.c
 */

#include "output_nfm.h"
#include "constants.h"
#include "audio_output_functions.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "ring_buffer.h"
#include "utilities.h"
#include "signal_handler.h"
#include "sample_conversion_functions.h"
#include "interleave_functions.h"
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

// --- Constants ---
#define NFM_AUDIO_RATE          48000
#define NFM_AUDIO_CHANNELS      2 // Output is Stereo (Mono duplicated)

// Rate Constraints
#define NFM_MIN_INPUT_RATE      16000.0
#define NFM_MAX_INPUT_RATE      192000.0
#define NFM_DEFAULT_INPUT_RATE  48000.0

// Standard FM (Ham/Marine/NOAA) = 5kHz deviation
#define DEV_STANDARD            5000.0f
// Narrow FM (FRS/GMRS/Business) = 2.5kHz deviation
#define DEV_NARROW              2500.0f

// De-emphasis Time Constant (75us)
#define NFM_DEEMPH_FREQ         2122.0f

// Brickwall Audio LPF (Voice Band cutoff)
#define NFM_AUDIO_CUTOFF        4000.0f

// Squelch Settings
#define SQ_HYSTERESIS_DB        2.0f
#define SQ_HANG_TIME_MS         250

// --- Context ---
typedef struct {
    // Pipeline
    AudioOutputContext* audio_out;

    // DSP
    freqdem fm_demod;           // Liquid FM Demodulator
    iirfilt_rrrf deemph_filter; // De-emphasis (The "Voice Filter")
    iirfilt_rrrf audio_lpf;     // Brickwall Audio LPF (4th Order)
    msresamp_rrrf resampler;    // Resampler (Input Rate -> 48kHz)

    // State
    float input_samplerate;
    float output_ratio;

    // Squelch State
    bool squelch_open;
    int squelch_hang_counter;
    int squelch_hang_max;

    // Buffers
    float* mono_buffer;         // Temp buffer for demodulated audio
    float* resamp_buffer;       // Temp buffer for resampled audio
    int16_t* pcm_out;           // Final Interleaved PCM
} NfmContext;

// --- Config ---
static struct {
    float gain;
    float squelch_snr;       // Squelch SNR threshold in dB
    bool squelch_disabled;    // 1 = disabled (Audio always open)

    bool is_narrow;           // 1 = Use 2.5kHz deviation (True NFM)
    bool disable_discriminator_filter; // 1 = Disable De-emphasis & LPF (Raw/Data Mode)
} s_nfm_config = {
    .gain = 1.0f,
    .squelch_snr = 10.0f,    // default: opens at 10.0 dB SNR (closes at 8.0 dB due to hysteresis)
    .squelch_disabled = 0,   // Default: Squelch is ENABLED (0)
    .is_narrow = 0,          // Default to Standard (5kHz dev)
    .disable_discriminator_filter = 0 // Default to Filter Enabled (Voice mode)
};

// --- Module Interface ---

static bool nfm_output_validate_options(AppConfig* config) {
    config->baseband_sample_format.format = CF32;

    // 1. Default to NFM_DEFAULT_INPUT_RATE (48k) if not specified
    if (config->baseband_sample_rate.rate_hz == 0.0) {
        config->baseband_sample_rate.rate_hz = NFM_DEFAULT_INPUT_RATE;
        config->baseband_sample_rate.provided = true;
    }

    // 2. Enforce Bounds
    double rate = config->baseband_sample_rate.rate_hz;
    if (rate < NFM_MIN_INPUT_RATE || rate > NFM_MAX_INPUT_RATE) {
        log_error("NFM: Invalid input rate %.15g Hz.", rate);
        log_error("Valid range is %.15g Hz to %.15g Hz.", NFM_MIN_INPUT_RATE, NFM_MAX_INPUT_RATE);
        return false;
    }

    return true;
}

static bool nfm_output_initialize(ModuleContext* context) {
    AppContext* res = context->app;
    NfmContext* p = (NfmContext*)mem_arena_alloc(&res->pipeline.setup_arena, sizeof(NfmContext), true);
    res->module.output_private_data = p;

    p->audio_out = audio_output_create(&res->pipeline.setup_arena, NFM_AUDIO_RATE, NFM_AUDIO_CHANNELS, res->module.source_info.demod_audio_buffer_size, context->config->audio.writer_path, context->config->audio.writer_rf64, context->config->audio.mute);
    if (!p->audio_out) return false;

    // 3. DSP Setup
    p->input_samplerate = (float)context->config->baseband_sample_rate.rate_hz;
    p->output_ratio = (float)NFM_AUDIO_RATE / p->input_samplerate;

    // A. Determine Deviation (Modulation Index)
    float selected_dev = s_nfm_config.is_narrow ? DEV_NARROW : DEV_STANDARD;
    float kf = selected_dev / p->input_samplerate;
    p->fm_demod = freqdem_create(kf);

    // B. De-emphasis Filter (75us)
    p->deemph_filter = iirfilt_rrrf_create_lowpass(1, NFM_DEEMPH_FREQ / p->input_samplerate);

    // C. Audio Cleanup Filter (4kHz)
    p->audio_lpf = iirfilt_rrrf_create_lowpass(4, NFM_AUDIO_CUTOFF / p->input_samplerate);

    // D. Output Resampler
    p->resampler = msresamp_rrrf_create(p->output_ratio, context->config->dsp.filter.args.attenuation);

    // Squelch state
    p->squelch_open = false;
    p->squelch_hang_counter = 0;
    p->squelch_hang_max = (int)(p->input_samplerate * (SQ_HANG_TIME_MS / 1000.0f));

    log_info("NFM: Baseband %.15g Hz | Mode: %s | Squelch: %.15g dB | Discriminator Filter: %s",
             p->input_samplerate,
             s_nfm_config.is_narrow ? "Narrow (2.5k)" : "Standard (5k)",
             s_nfm_config.squelch_snr,
             s_nfm_config.disable_discriminator_filter ? "Disabled" : "Enabled");

    // 4. Buffers
    size_t in_samples = res->pipeline.alloc_size_samples;
    size_t out_samples = (size_t)ceil(in_samples * p->output_ratio) + 64;

    p->mono_buffer   = mem_arena_alloc(&res->pipeline.setup_arena, in_samples * sizeof(float), false);
    p->resamp_buffer = mem_arena_alloc(&res->pipeline.setup_arena, out_samples * sizeof(float), false);
    p->pcm_out       = mem_arena_alloc(&res->pipeline.setup_arena, out_samples * 2 * sizeof(int16_t), false);

    return true;
}

static void nfm_output_reset(ModuleContext* context) { (void)context; }
static void nfm_output_flush(ModuleContext* context) {
    NfmContext* p = (NfmContext*)context->app->module.output_private_data;
    audio_output_clear(p->audio_out);
}
static size_t nfm_output_write_chunk(ModuleContext* context, const void* buffer, size_t input_bytes) {
    AppContext* res = context->app;
    NfmContext* p = (NfmContext*)res->module.output_private_data;

    static size_t stat_counter = 0;
    static double accum_mag_sum = 0.0, accum_mag_sq_sum = 0.0;
    static size_t stat_rate_threshold = 0;
    static bool _first_run = true;
    if (_first_run) {
        stat_rate_threshold = (size_t)(p->input_samplerate * CONSOLE_UPDATE_INTERVAL_SEC);
        _first_run = false;
    }
    if (input_bytes == 0) return 0;

    unsigned int n = input_bytes / res->module.output_bytes_per_iq_sample;
    liquid_float_complex* iq = (liquid_float_complex*)buffer;

    // 1. Calculate block-level sum of magnitudes and sum of squares
    float block_mag_sum = 0.0f;
    float block_mag_sq_sum = 0.0f;
    for (unsigned int i = 0; i < n; i++) {
        float mag2 = crealf(iq[i])*crealf(iq[i]) + cimagf(iq[i])*cimagf(iq[i]);
        block_mag_sq_sum += mag2;
        block_mag_sum += sqrtf(mag2);
    }

    // Accumulate for periodic status logging
    accum_mag_sum += block_mag_sum;
    accum_mag_sq_sum += block_mag_sq_sum;
    stat_counter += n;

    // 2. Calculate exact block-level SNR
    float block_mean_mag = block_mag_sum / (float)n;
    float block_avg_power = block_mag_sq_sum / (float)n;
    float block_variance = block_avg_power - (block_mean_mag * block_mean_mag);

    float block_snr_db = 0.0f;
    if (block_variance > 1e-12f) {
        block_snr_db = 10.0f * log10f((block_mean_mag * block_mean_mag) / block_variance);
    } else {
        block_snr_db = 100.0f; // Perfect, noiseless signal limit (DC)
    }

    // 3. SNR-based Squelch Logic (Using Hysteresis and Hang-time)
    if (!s_nfm_config.squelch_disabled) {
        // Open squelch immediately if block SNR rises above threshold (e.g., 10.0 dB)
        if (block_snr_db > s_nfm_config.squelch_snr) {
            p->squelch_open = true;
            p->squelch_hang_counter = p->squelch_hang_max;
        }
        // If signal drops, count down the squelch tail (hang-time)
        else if (p->squelch_hang_counter > 0) {
            p->squelch_hang_counter -= n;
        }
        // Close squelch only if SNR drops below threshold minus hysteresis (e.g., 10.0 - 2.0 = 8.0 dB)
        else if (block_snr_db < (s_nfm_config.squelch_snr - SQ_HYSTERESIS_DB)) {
            p->squelch_open = false;
        }
    } else {
        p->squelch_open = true;
    }

    // 4. Periodic console logging (unchanged, rates aligned to CONSOLE_UPDATE_INTERVAL)
    if (stat_counter >= stat_rate_threshold) {
        double avg_power = accum_mag_sq_sum / (double)stat_counter;
        float dbfs = 10.0f * log10f((float)avg_power + 1e-12f);

        if (p->squelch_open) {
            double mean_mag = accum_mag_sum / (double)stat_counter;
            float snr_db = 10.0f * log10f((float)((mean_mag*mean_mag) / fmax(1e-12, avg_power - (mean_mag*mean_mag))));
            log_info("dBFS: %5.1f | SNR: %4.1f dB | Squelch: OPEN", dbfs, snr_db);
        } else {
            log_info("dBFS: %5.1f | Squelch: CLOSED", dbfs);
        }
        stat_counter = 0; accum_mag_sum = 0.0; accum_mag_sq_sum = 0.0;
    }

    // 5. Demodulate and apply DSP filters
    freqdem_demodulate_block(p->fm_demod, iq, n, p->mono_buffer);
    for (unsigned int i = 0; i < n; i++) {
        float sample = p->mono_buffer[i];
        if (!s_nfm_config.disable_discriminator_filter) {
            iirfilt_rrrf_execute(p->deemph_filter, sample, &sample);
            iirfilt_rrrf_execute(p->audio_lpf, sample, &sample);
        }
        sample *= s_nfm_config.gain;
        if (!p->squelch_open) sample = 0.0f; // Mute silent buffers cleanly
        p->mono_buffer[i] = sample;
    }

    // 6. Resample to 48kHz audio and output
    unsigned int num_resampled;
    msresamp_rrrf_execute(p->resampler, p->mono_buffer, n, p->resamp_buffer, &num_resampled);
    interleave_f32_to_s16(p->resamp_buffer, p->resamp_buffer, p->pcm_out, num_resampled);
    audio_output_write(p->audio_out, p->pcm_out, num_resampled * 2 * sizeof(int16_t), res->pipeline_mode);

    return input_bytes;
}

static void nfm_output_cleanup(ModuleContext* context) {
    AppContext* res = context->app;
    if (!res->module.output_private_data) return;
    NfmContext* p = (NfmContext*)res->module.output_private_data;

    audio_output_destroy(p->audio_out);
    if (p->fm_demod) freqdem_destroy(p->fm_demod);
    if (p->deemph_filter) iirfilt_rrrf_destroy(p->deemph_filter);
    if (p->audio_lpf) iirfilt_rrrf_destroy(p->audio_lpf);
    if (p->resampler) msresamp_rrrf_destroy(p->resampler);
}

static void nfm_output_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
    const char* mode = s_nfm_config.is_narrow ? "Narrow (2.5k Dev)" : "Standard (5k Dev)";
    const char* type = s_nfm_config.disable_discriminator_filter ? "Discriminator Filter Disabled" : "Discriminator Filter Enabled";

    add_summary_item(info, "Output Type", "NFM");
    add_summary_item(info, "Mode", "%s", mode);
    add_summary_item(info, "Audio", "%s", type);
    add_summary_item(info, "Squelch", "%.15g dB", s_nfm_config.squelch_snr);
}

static const struct argparse_option nfm_output_cli_options[] = {
    OPT_GROUP("NFM Output (nfm)"),
    OPT_FLOAT(0, "nfm-gain", &s_nfm_config.gain, "Audio gain (default: 1.0)", NULL, 0, 0),
    OPT_FLOAT(0, "nfm-squelch", &s_nfm_config.squelch_snr, "Squelch SNR threshold in dB (default: 10.0)", NULL, 0, 0),
    OPT_BOOLEAN(0, "nfm-narrow", &s_nfm_config.is_narrow, "Enable Narrow mode (2.5kHz dev). Default is Standard (5kHz dev).", NULL, 0, 0),
    OPT_BOOLEAN(0, "nfm-no-squelch", &s_nfm_config.squelch_disabled, "Disable squelch (force open audio)", NULL, 0, 0),
    OPT_BOOLEAN(0, "nfm-no-discriminator-filter", &s_nfm_config.disable_discriminator_filter, "Disable the discriminator filter.", NULL, 0, 0),
};

const struct argparse_option* nfm_output_get_cli_options(int* count) {
    *count = sizeof(nfm_output_cli_options) / sizeof(nfm_output_cli_options[0]);
    return nfm_output_cli_options;
}

static OutputModuleInterface s_nfm_output_api = {
    .initialize = nfm_output_initialize,
    .write_chunk = nfm_output_write_chunk,
    .reset = nfm_output_reset,
    .flush = nfm_output_flush,
    .cleanup = nfm_output_cleanup,
    .get_summary_info = nfm_output_get_summary_info,
    .validate_options = nfm_output_validate_options,
    .get_cli_options = nfm_output_get_cli_options,
};

OutputModuleInterface* output_nfm_get_module_api(void) {
    return &s_nfm_output_api;
}
