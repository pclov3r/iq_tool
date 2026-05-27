#include "output_am.h"
#include "constants.h"
#include "audio_output_functions.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "signal_handler.h"
#include "utils.h"
#include "queue.h"
#include "sample_convert.h"
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
#define AUDIO_BUFFER_SIZE       (1536 * 1024)
#define MAX_AUDIO_CUTOFF_HZ     20000.0f

// --- Constraints ---
#define AM_MIN_INPUT_RATE       12000.0
#define AM_MAX_INPUT_RATE       192000.0
#define AM_DEFAULT_INPUT_RATE   48000.0

// --- AGC Tuning (Universal Mode) ---
#define AM_AGC_ATTACK_BW        1.0e-2f // Fast attack (2ms)

// Decay: Medium-Slow (1.0s).
// Used only AFTER the Hold Timer expires.
#define AM_AGC_DECAY_BW         2.0e-5f

// Hold Time: 0.5 Seconds.
// The AGC will FREEZE gain for this long after a signal drop.
#define AM_AGC_HOLD_SEC         0.5f

#define AM_AGC_HANG_BW          1.0e-8f // Hang on silence
#define AM_AGC_NOISE_THRESH     1.0e-4f // Noise floor threshold (-80dB)
#define AM_AGC_INITIAL_GAIN     316.0f  // Start at 50dB
#define AM_AGC_MAX_GAIN         30.0f   // Max gain +30dB
#define AM_AGC_TARGET_LEVEL     0.3f    // Target output level

// --- PLL Tuning (Sync AM) ---
// Acquisition: Wide to catch signal (100 Hz)
#define AM_PLL_ACQ_BW           100.0f
// Tracking: 30 Hz
#define AM_PLL_TRACKING_BW      30.0f
// Lock Time: 0.5 seconds
#define AM_PLL_LOCK_TIME        0.5f

// --- Lock Detector / Auto-Fallback ---
// Quality Threshold (0.0 - 1.0). Ratio of In-Phase energy to Total Magnitude.
#define AM_LOCK_THRESHOLD       0.5f

// Timeout (seconds). Force fallback to Envelope if unlocked for this long.
#define AM_LOCK_TIMEOUT_SEC     3


// --- Internal Structures ---

typedef struct {
    // Pipeline State
    AudioOutputContext* audio_out;

    // DSP Objects (Liquid)
    nco_crcf pll;               // Sync AM PLL

    // Manual DC Tracking (Input Rate)
    float dc_integrator;
    float dc_alpha;

    // Filters (Output Rate)
    firfilt_rrrf audio_lpf;     // FIR Linear Phase Filter (Fixed at 48k)

    // Liquid AGC Object (Output Rate)
    agc_rrrf agc;
    float agc_attack_bw;
    float agc_decay_bw;

    // AGC Hang State
    size_t agc_hang_counter;    // Samples remaining to hold gain
    size_t agc_hang_max;        // Reset value for counter

    // Output Resamplers (Input Rate -> 48k)
    msresamp_rrrf resamp_out;
    float output_resample_ratio;

    // Processing State
    float input_samplerate;
    float manual_gain;
    bool sync_mode;
    bool pll_tracking_mode;
    size_t pll_lock_counter;

    // Auto-Fallback State
    bool fallback_mode;         // True if we gave up on Sync AM
    int unlock_counter;         // Counts consecutive seconds of bad lock

    // Scratch Buffers
    float* mono_buffer;
    float* resamp_buffer;
    int16_t* interleaved_pcm;
} AmContext;

// --- CLI Config ---
static struct {
    float gain_val;         // Linear gain
    float audio_cutoff;     // Audio LPF cutoff in Hz
    int force_envelope;     // Force Envelope Detection (Disable Sync)
} s_am_config = {
    .gain_val = 1.0f,
    .audio_cutoff = 10000.0f, // Default: 10kHz (Standard AM Broadcast Bandwidth)
    .force_envelope = 0       // Default: 0 (Sync AM is Active)
};

// --- Module Interface Implementation ---

static bool am_output_validate_options(AppConfig* config) {
    config->output.sample_format = CF32;

    if (config->output_sample_rate.rate_hz == 0.0) {
        config->output_sample_rate.rate_hz = AM_DEFAULT_INPUT_RATE;
        config->output_sample_rate.provided = true;
        log_debug("AM: No rate specified. Requesting %.15g Hz.", AM_DEFAULT_INPUT_RATE);
    } else {
        double rate = config->output_sample_rate.rate_hz;
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

static bool am_output_initialize(ModuleContext* ctx) {
    AppContext* res = ctx->app;

    AmContext* p = (AmContext*)mem_arena_alloc(&res->pipeline.setup_arena, sizeof(AmContext), true);
    if (!p) return false;
    res->module.output_private_data = p;

    p->audio_out = audio_output_create(&res->pipeline.setup_arena, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BUFFER_SIZE, ctx->config->dsp.audio_writer_path, ctx->config->dsp.audio_writer_rf64, ctx->config->dsp.mute_audio);
    if (!p->audio_out) return false;

    // 3. DSP Configuration
    float input_rate = (float)ctx->config->output_sample_rate.rate_hz;
    if (input_rate < 1.0f) input_rate = 48000.0f;

    p->input_samplerate = input_rate;
    p->manual_gain = s_am_config.gain_val;
    p->sync_mode = !s_am_config.force_envelope;
    p->fallback_mode = false;
    p->unlock_counter = 0;

    if (s_am_config.audio_cutoff > MAX_AUDIO_CUTOFF_HZ) {
        s_am_config.audio_cutoff = MAX_AUDIO_CUTOFF_HZ;
    }

    log_info("AM: Input Rate %.15g Hz | Audio Cutoff: %.15g Hz | Mode: %s",
             input_rate, s_am_config.audio_cutoff,
             p->sync_mode ? "Synchronous (PLL)" : "Envelope (Magnitude)");

    // 4. Create Liquid Objects

    // A. Demodulators (PLL)
    if (p->sync_mode) {
        p->pll = nco_crcf_create(LIQUID_VCO);
        nco_crcf_pll_set_bandwidth(p->pll, AM_PLL_ACQ_BW / input_rate);
    } else {
        p->pll = NULL;
    }

    // B. Manual DC Tracking
    float dc_cutoff = 5.0f;
    p->dc_alpha = expf(-2.0f * (float)M_PI * dc_cutoff / input_rate);
    p->dc_integrator = 0.0f;

    // C. Resampler
    p->output_resample_ratio = (float)AUDIO_SAMPLE_RATE / input_rate;
    p->resamp_out = msresamp_rrrf_create(p->output_resample_ratio, ctx->config->dsp.filter.args.attenuation);

    // D. Audio Lowpass Filter
    unsigned int h_len = 63;
    float h[63];
    float fc = s_am_config.audio_cutoff / (float)AUDIO_SAMPLE_RATE;
    if (fc > 0.49f) fc = 0.49f;
    liquid_firdes_kaiser(h_len, fc, ctx->config->dsp.filter.args.attenuation, 0.0f, h);
    p->audio_lpf = firfilt_rrrf_create(h, h_len);

    // E. AGC
    p->agc = agc_rrrf_create();
    agc_rrrf_set_scale(p->agc, AM_AGC_TARGET_LEVEL);

    // Set Initial Gain to 50dB (316.0)
    agc_rrrf_set_gain(p->agc, AM_AGC_INITIAL_GAIN);

    p->agc_attack_bw = AM_AGC_ATTACK_BW;
    p->agc_decay_bw = AM_AGC_DECAY_BW;
    agc_rrrf_set_bandwidth(p->agc, p->agc_attack_bw);

    // Setup Hang Timer (Samples at 48kHz)
    p->agc_hang_max = (size_t)(AUDIO_SAMPLE_RATE * AM_AGC_HOLD_SEC);
    p->agc_hang_counter = 0;

    // 5. Scratch Buffers
    size_t buf_samples = res->pipeline.alloc_size_samples;
    size_t out_buf_samples = (size_t)ceil(buf_samples * p->output_resample_ratio) + 128;
    size_t max_dsp_samples = (buf_samples > out_buf_samples) ? buf_samples : out_buf_samples;

    p->mono_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    p->resamp_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    p->interleaved_pcm = mem_arena_alloc(&res->pipeline.setup_arena, out_buf_samples * 2 * sizeof(int16_t), false);

    if (!p->mono_buffer || !p->resamp_buffer || !p->interleaved_pcm) return false;

    

    return true;
}


static void am_output_reset(ModuleContext* ctx) { (void)ctx; /* TODO: Reset PLL state */ }
static void am_output_flush(ModuleContext* ctx) {
    AmContext* p = (AmContext*)ctx->app->module.output_private_data;
    audio_output_flush(p->audio_out);
}
static size_t am_output_write_chunk(ModuleContext* ctx, const void* buffer, size_t input_bytes) {
    AppContext* res = ctx->app;
    AmContext* p = (AmContext*)res->module.output_private_data;

    

    const float pcm_scale = 32767.0f;

    static size_t stat_counter = 0;
    static double accum_mag_sq_sum = 0.0, accum_phase_err_sq_sum = 0.0, accum_carrier_freq_sum = 0.0;
    static double accum_carrier_strength_sum = 0.0, accum_inphase_sum = 0.0;
    static size_t accum_pll_count = 0;
    
    static size_t stat_threshold = 0;
    static size_t pll_lock_samples = 0;
    static bool _first_run = true;
    if (_first_run) {
        stat_threshold = (size_t)(p->input_samplerate * CONSOLE_UPDATE_INTERVAL_SEC);
        pll_lock_samples = (size_t)(p->input_samplerate * AM_PLL_LOCK_TIME);
        _first_run = false;
    }

    if (input_bytes == 0) return 0;
    unsigned int num_frames = input_bytes / res->module.output_bytes_per_iq_sample;
    liquid_float_complex* iq_ptr = (liquid_float_complex*)buffer;
    bool use_sync_mode = p->sync_mode && !p->fallback_mode;

    for (unsigned int i = 0; i < num_frames; i++) {
        float raw_demod = 0.0f;
        float mag = cabsf(iq_ptr[i]);
        accum_mag_sq_sum += (mag * mag);
        if (use_sync_mode) {
            if (!p->pll_tracking_mode && p->pll_lock_counter++ >= pll_lock_samples) {
                nco_crcf_pll_set_bandwidth(p->pll, AM_PLL_TRACKING_BW / p->input_samplerate);
                p->pll_tracking_mode = true;
            }
            liquid_float_complex carrier, product;
            nco_crcf_cexpf(p->pll, &carrier);
            product = iq_ptr[i] * conjf(carrier);
            float phase_error = cimagf(product);
            nco_crcf_pll_step(p->pll, phase_error);
            nco_crcf_step(p->pll);
            raw_demod = crealf(product);
            if ((i & 3) == 0) {
                accum_phase_err_sq_sum += (phase_error * phase_error);
                accum_carrier_freq_sum += (nco_crcf_get_frequency(p->pll) / (2.0f * (float)M_PI)) * p->input_samplerate;
                accum_carrier_strength_sum += cabsf(product);
                accum_inphase_sum += raw_demod;
                accum_pll_count++;
            }
        } else { raw_demod = mag; }
        p->dc_integrator = (p->dc_alpha * p->dc_integrator) + ((1.0f - p->dc_alpha) * raw_demod);
        p->mono_buffer[i] = raw_demod - p->dc_integrator;
    }
    stat_counter += num_frames;

        if (stat_counter >= stat_threshold) {
        float dbfs = 10.0f * log10f((float)(accum_mag_sq_sum / (double)stat_counter) + 1e-10f);
        float agc_rssi = agc_rrrf_get_rssi(p->agc);

        if (use_sync_mode && accum_pll_count > 0) {
            float lock_quality = (float)(accum_inphase_sum / (accum_carrier_strength_sum + 1e-9));
            if (lock_quality < AM_LOCK_THRESHOLD) {
                if (++p->unlock_counter >= AM_LOCK_TIMEOUT_SEC) {
                    p->fallback_mode = true;
                    log_warn("AM: PLL failed to lock. Falling back to Envelope detection.");
                }
            } else { p->unlock_counter = 0; }
            log_info("dBFS: %5.1f | AGC Gain: %4.1f dB | Offset: %5.2f Hz | PLL Lock: %5.1f%%",
                     dbfs, -agc_rssi, (float)(accum_carrier_freq_sum / (double)accum_pll_count), fmaxf(0.0f, lock_quality) * 100.0f);
            accum_phase_err_sq_sum=0.0; accum_carrier_freq_sum=0.0; accum_carrier_strength_sum=0.0; accum_inphase_sum=0.0; accum_pll_count=0;
        } else { log_info("dBFS: %5.1f | AGC Gain: %4.1f dB", dbfs, -agc_rssi); }
        stat_counter = 0; accum_mag_sq_sum = 0.0;
    }

    unsigned int num_resampled;
    msresamp_rrrf_execute(p->resamp_out, p->mono_buffer, num_frames, p->resamp_buffer, &num_resampled);

    for (unsigned int i = 0; i < num_resampled; i++) {
        float sample = p->resamp_buffer[i];
        firfilt_rrrf_push(p->audio_lpf, sample);
        firfilt_rrrf_execute(p->audio_lpf, &sample);
        if (fabsf(sample) > agc_rrrf_get_signal_level(p->agc)) {
            agc_rrrf_set_bandwidth(p->agc, p->agc_attack_bw);
            p->agc_hang_counter = p->agc_hang_max;
        } else if (p->agc_hang_counter > 0) {
            p->agc_hang_counter--; agc_rrrf_set_bandwidth(p->agc, AM_AGC_HANG_BW);
        } else {
            agc_rrrf_set_bandwidth(p->agc, (fabsf(sample) > AM_AGC_NOISE_THRESH) ? p->agc_decay_bw : AM_AGC_HANG_BW);
        }
        agc_rrrf_execute(p->agc, sample, &sample);
        sample *= p->manual_gain;
        int16_t pcm = (int16_t)(fmaxf(-1.0f, fminf(1.0f, sample)) * pcm_scale);
        p->interleaved_pcm[2*i] = pcm;
        p->interleaved_pcm[2*i + 1] = pcm;
    }

    audio_output_write(p->audio_out, p->interleaved_pcm, num_resampled * 2 * sizeof(int16_t), res->pipeline_mode);
    return input_bytes;
}


static void am_output_cleanup(ModuleContext* ctx) {
    AppContext* res = ctx->app;
    if (!res->module.output_private_data) return;
    AmContext* p = (AmContext*)res->module.output_private_data;

    audio_output_destroy(p->audio_out);

    if (p->pll) nco_crcf_destroy(p->pll);
    if (p->agc) agc_rrrf_destroy(p->agc);
    if (p->audio_lpf) firfilt_rrrf_destroy(p->audio_lpf);
    if (p->resamp_out) msresamp_rrrf_destroy(p->resamp_out);
}

static void am_output_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "AM Audio");
    add_summary_item(info, "Mode", "%s", s_am_config.force_envelope ? "Envelope (Mag)" : "Synchronous (PLL)");
    add_summary_item(info, "Audio Sample Rate", "%d Hz", AUDIO_SAMPLE_RATE);
    add_summary_item(info, "Filter Cutoff", "%.15g Hz", s_am_config.audio_cutoff);
}

static const struct argparse_option am_output_cli_options[] = {
    OPT_GROUP("AM Output (am)"),
    OPT_FLOAT(0, "am-gain", &s_am_config.gain_val, "Set audio output gain (linear).", NULL, 0, 0),
    OPT_FLOAT(0, "am-cutoff", &s_am_config.audio_cutoff, "Set audio lowpass filter cutoff in Hz (default: 10000).", NULL, 0, 0),
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
