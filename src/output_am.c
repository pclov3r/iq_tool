#include "output_am.h"
#include "miniaudio.h"
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
#define AUDIO_BUFFER_SIZE       (512 * 1024)
#define AM_STATS_INTERVAL_SEC   1.0f
#define MAX_AUDIO_CUTOFF_HZ     20000.0f

// --- Constraints ---
#define AM_MIN_INPUT_RATE       12000.0
#define AM_MAX_INPUT_RATE       192000.0
#define AM_DEFAULT_INPUT_RATE   48000.0

// --- Internal Structures ---

typedef struct {
    // Pipeline State
    ma_device audio_device;
    RingBuffer* audio_ring_buffer;
    bool audio_device_initialized;

    // DSP Objects (Liquid)
    // Note: 'ampmodem' removed to prevent hollow sound.
    // We use raw magnitude calculation instead.
    nco_crcf pll;               // Sync AM PLL

    // Manual DC Tracking (Input Rate)
    // REQUIRED: Since we aren't using ampmodem, we must remove DC manually.
    float dc_integrator;
    float dc_alpha;

    // Filters (Output Rate)
    firfilt_rrrf audio_lpf;     // FIR Linear Phase Filter (Fixed at 48k)

    // Liquid AGC Object (Output Rate)
    agc_rrrf agc;
    float agc_attack_bw;
    float agc_decay_bw;

    // Output Resamplers (Input Rate -> 48k)
    msresamp_rrrf resamp_out;
    float output_resample_ratio;

    // Processing State
    float input_samplerate;
    float manual_gain;
    bool sync_mode;
    bool pll_tracking_mode;
    size_t pll_lock_counter;

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
    .gain_val = 2.5f,
    .audio_cutoff = 10000.0f, // Default: 10kHz (Standard AM Broadcast Bandwidth)
    .force_envelope = 0       // Default: 0 (Sync AM is Active)
};

// --- Miniaudio Callback ---
static void miniaudio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    AmContext* ctx = (AmContext*)pDevice->pUserData;
    if (frameCount == 0) return;

    size_t bytes_needed = frameCount * AUDIO_CHANNELS * sizeof(int16_t);
    size_t available = ring_buffer_get_size(ctx->audio_ring_buffer);

    if (available < bytes_needed) {
        memset(pOutput, 0, bytes_needed);
        return;
    }
    ring_buffer_read(ctx->audio_ring_buffer, pOutput, bytes_needed);
}

// --- Module Interface Implementation ---

static bool am_validate_options(AppConfig* config) {
    // 1. Force CF32 (Standard)
    config->output.format = CF32;

    // 2. Handle Rate "Contract" with Pipeline
    if (config->output_rate.target_rate == 0.0) {
        // Case A: User didn't specify.
        // Force 48kHz. This creates a 1:1 relationship with audio output.
        // This is the most efficient mode (resampler becomes pass-through).
        config->output_rate.target_rate = AM_DEFAULT_INPUT_RATE;
        config->output_rate.provided = true;

        // Changed to LOG_DEBUG to reduce startup noise.
        // The user will see the final rate in the initialization log.
        log_debug("AM: No rate specified. Requesting %.0f Hz.", AM_DEFAULT_INPUT_RATE);
    } else {
        // Case B: User specified a rate. Enforce sanity limits.
        double rate = config->output_rate.target_rate;

        // Minimum: 12k (below this, speech is unintelligible)
        if (rate < AM_MIN_INPUT_RATE) {
            log_fatal("AM: Input rate %.0f Hz is too low for audio (Min: %.0f).", rate, AM_MIN_INPUT_RATE);
            return false;
        }

        // Maximum: 192k.
        // Demodulating AM at 1MS/s or 2MS/s is a massive waste of CPU for no benefit.
        if (rate > AM_MAX_INPUT_RATE) {
            log_fatal("AM: Input rate %.0f Hz is unnecessarily high (Max: %.0f).", rate, AM_MAX_INPUT_RATE);
            log_fatal("AM: High sample rates waste CPU with no audio benefit.");
            log_fatal("AM: Please use --output-rate 48000 for best performance.");
            return false;
        }
    }
    return true;
}

static bool am_initialize(ModuleContext* ctx) {
    AppContext* res = ctx->app;

    AmContext* p = (AmContext*)mem_arena_alloc(&res->pipeline.setup_arena, sizeof(AmContext), true);
    if (!p) return false;
    res->modules.output_private_data = p;

    // 1. Setup Audio Ring Buffer
    p->audio_ring_buffer = ring_buffer_create(AUDIO_BUFFER_SIZE);
    if (!p->audio_ring_buffer) return false;

    // 2. Setup Miniaudio
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_s16;
    deviceConfig.playback.channels = AUDIO_CHANNELS;
    deviceConfig.sampleRate        = AUDIO_SAMPLE_RATE;
    deviceConfig.dataCallback      = miniaudio_data_callback;
    deviceConfig.pUserData         = p;

    if (ma_device_init(NULL, &deviceConfig, &p->audio_device) != MA_SUCCESS) {
        log_fatal("AM: Failed to initialize audio device.");
        return false;
    }
    p->audio_device_initialized = true;

    // 3. DSP Configuration
    float input_rate = (float)ctx->config->output_rate.target_rate;
    // Sanity fallback (should be caught by validate_options)
    if (input_rate < 1.0f) input_rate = 48000.0f;

    p->input_samplerate = input_rate;
    p->manual_gain = s_am_config.gain_val;
    p->sync_mode = !s_am_config.force_envelope;

    if (s_am_config.audio_cutoff > MAX_AUDIO_CUTOFF_HZ) {
        s_am_config.audio_cutoff = MAX_AUDIO_CUTOFF_HZ;
    }

    log_info("AM: Input Rate %.0f Hz | Audio Cutoff: %.0f Hz | Mode: %s",
             input_rate, s_am_config.audio_cutoff,
             p->sync_mode ? "Synchronous (PLL)" : "Envelope (Magnitude)");

    // 4. Create Liquid Objects

    // A. Demodulators
    // We only create the PLL. Envelope mode uses raw math (cabsf).
    if (p->sync_mode) {
        p->pll = nco_crcf_create(LIQUID_VCO);
        nco_crcf_pll_set_bandwidth(p->pll, 100.0f / input_rate);
    } else {
        p->pll = NULL;
    }

    // B. Manual DC Tracking (Input Rate)
    // This is CRITICAL. Since we aren't using ampmodem, we must remove the Carrier (DC) manually.
    // We calculate the alpha based on the *actual* input rate.
    float dc_cutoff = 5.0f;
    p->dc_alpha = expf(-2.0f * (float)M_PI * dc_cutoff / input_rate);
    p->dc_integrator = 0.0f;

    // C. Resampler
    p->output_resample_ratio = (float)AUDIO_SAMPLE_RATE / input_rate;
    p->resamp_out = msresamp_rrrf_create(p->output_resample_ratio, 60.0f);

    // D. Audio Lowpass Filter (Output Rate - 48kHz)
    unsigned int h_len = 63;
    float h[63];
    float fc = s_am_config.audio_cutoff / (float)AUDIO_SAMPLE_RATE;
    if (fc > 0.49f) fc = 0.49f;
    liquid_firdes_kaiser(h_len, fc, 60.0f, 0.0f, h);
    p->audio_lpf = firfilt_rrrf_create(h, h_len);

    // E. AGC (Output Rate)
    p->agc = agc_rrrf_create();
    agc_rrrf_set_scale(p->agc, 0.3f);
    agc_rrrf_set_gain(p->agc, 100.0f);

    p->agc_attack_bw = 0.1f;
    p->agc_decay_bw = 1e-4f;
    agc_rrrf_set_bandwidth(p->agc, p->agc_attack_bw);

    // 5. Scratch Buffers
    size_t buf_samples = res->pipeline.alloc_size_samples;
    size_t out_buf_samples = (size_t)ceil(buf_samples * p->output_resample_ratio) + 128;
    size_t max_dsp_samples = (buf_samples > out_buf_samples) ? buf_samples : out_buf_samples;

    p->mono_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    p->resamp_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    p->interleaved_pcm = mem_arena_alloc(&res->pipeline.setup_arena, out_buf_samples * 2 * sizeof(int16_t), false);

    if (!p->mono_buffer || !p->resamp_buffer || !p->interleaved_pcm) return false;

    // 6. Start Audio
    if (ma_device_start(&p->audio_device) != MA_SUCCESS) {
        log_error("AM: Failed to start audio callback.");
        return false;
    }

    return true;
}

static void* am_run_writer(ModuleContext* ctx) {
    AppContext* res = ctx->app;
    AmContext* p = (AmContext*)res->modules.output_private_data;

    const size_t THROTTLE_THRESHOLD = (size_t)(AUDIO_BUFFER_SIZE * 0.8);

    size_t stat_counter = 0;
    size_t stat_threshold = (size_t)(p->input_samplerate * AM_STATS_INTERVAL_SEC);
    double accum_mag_sq_sum = 0.0;

    // PLL stats
    double accum_phase_err_sq_sum = 0.0;
    double accum_carrier_freq_sum = 0.0;
    double accum_carrier_strength_sum = 0.0;
    size_t accum_pll_count = 0;

    const size_t pll_lock_samples = (size_t)(p->input_samplerate * 0.5f);
    p->pll_tracking_mode = false;
    p->pll_lock_counter = 0;

    const float pcm_scale = 32767.0f;

    while (true) {
        if (p->audio_ring_buffer) {
            while (ring_buffer_get_size(p->audio_ring_buffer) > THROTTLE_THRESHOLD) {
                #ifdef _WIN32
                    Sleep(10);
                #else
                    usleep(10000);
                #endif
                if (is_shutdown_requested()) goto cleanup;
            }
        }

        SampleChunk* item = (SampleChunk*)queue_dequeue(res->pipeline.writer_input_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
            queue_enqueue(res->pipeline.free_sample_chunk_queue, item);
            continue;
        }

        if (item->is_last_chunk) {
            utils_wait_for_ring_buffer_drain(p->audio_ring_buffer, 10, 200, 200);
            queue_enqueue(res->pipeline.free_sample_chunk_queue, item);
            break;
        }

        if (item->frames_to_write > 0) {
            complex_float_t* iq_in = (complex_float_t*)item->final_output_data;
            unsigned int num_frames = item->frames_to_write;
            liquid_float_complex* iq_ptr = (liquid_float_complex*)iq_in;

            // --- STAGE 1: Demodulation (Input Rate) ---
            for (unsigned int i = 0; i < num_frames; i++) {
                float raw_demod = 0.0f;
                // Calculate Magnitude (Required for stats AND Envelope Mode)
                float mag = cabsf(iq_ptr[i]);
                accum_mag_sq_sum += (mag * mag);

                if (p->sync_mode) {
                    // --- SYNC MODE (PLL) ---
                    if (!p->pll_tracking_mode) {
                        p->pll_lock_counter++;
                        if (p->pll_lock_counter >= pll_lock_samples) {
                            float pll_tracking_bw = 20.0f / p->input_samplerate;
                            nco_crcf_pll_set_bandwidth(p->pll, pll_tracking_bw);
                            p->pll_tracking_mode = true;
                        }
                    }

                    liquid_float_complex carrier;
                    nco_crcf_cexpf(p->pll, &carrier);
                    liquid_float_complex product = iq_ptr[i] * conjf(carrier);

                    raw_demod = crealf(product);

                    float phase_error = cimagf(product);
                    nco_crcf_pll_step(p->pll, phase_error);
                    nco_crcf_step(p->pll);

                    if ((i & 3) == 0) {
                        accum_phase_err_sq_sum += (phase_error * phase_error);
                        accum_carrier_freq_sum += (nco_crcf_get_frequency(p->pll) / (2.0f * (float)M_PI)) * p->input_samplerate;
                        accum_carrier_strength_sum += cabsf(product);
                        accum_pll_count++;
                    }
                } else {
                    // --- ENVELOPE MODE (Magnitude) ---
                    // REPLACED ampmodem with direct magnitude calculation.
                    // This fixes the hollow sound caused by frequency offsets.
                    raw_demod = mag;
                }

                // --- STAGE 2: Soft DC Removal (Input Rate) ---
                // CRITICAL: Magnitude detection creates a HUGE DC offset (the carrier).
                // We MUST remove it here, or the audio will be broken.
                p->dc_integrator = (p->dc_alpha * p->dc_integrator) + ((1.0f - p->dc_alpha) * raw_demod);
                p->mono_buffer[i] = raw_demod - p->dc_integrator;
            }

            stat_counter += num_frames;

            // --- Stats ---
            if (stat_counter >= stat_threshold) {
                double avg_power = accum_mag_sq_sum / (double)stat_counter;
                float rssi_db = 10.0f * log10f((float)avg_power + 1e-10f);
                float agc_rssi = agc_rrrf_get_rssi(p->agc);

                if (p->sync_mode && accum_pll_count > 0) {
                    float avg_phase_mse = (float)(accum_phase_err_sq_sum / (double)accum_pll_count);
                    float phase_err_pct = sqrtf(avg_phase_mse) * 100.0f;
                    float avg_carrier_offset = (float)(accum_carrier_freq_sum / (double)accum_pll_count);
                    log_info("AM RSSI: %.1f dB | AGC: %.1f dB | Offset: %.2f Hz | PhaseErr: %.2f%%",
                             rssi_db, -agc_rssi, avg_carrier_offset, phase_err_pct);
                    accum_phase_err_sq_sum = 0.0; accum_carrier_freq_sum = 0.0; accum_carrier_strength_sum = 0.0; accum_pll_count = 0;
                } else {
                    log_info("AM RSSI: %.1f dB | AGC: %.1f dB | Mode: Envelope",
                             rssi_db, -agc_rssi);
                }
                stat_counter = 0; accum_mag_sq_sum = 0.0;
            }

            // --- STAGE 3: Resample (Input -> 48k) ---
            unsigned int num_resampled;
            msresamp_rrrf_execute(p->resamp_out, p->mono_buffer, num_frames, p->resamp_buffer, &num_resampled);

            // --- STAGE 4: Audio Processing (48k) ---
            for (unsigned int i = 0; i < num_resampled; i++) {
                float sample = p->resamp_buffer[i];

                // A. Audio LPF (FIR - Linear Phase)
                firfilt_rrrf_push(p->audio_lpf, sample);
                firfilt_rrrf_execute(p->audio_lpf, &sample);

                // B. AGC
                float current_est = agc_rrrf_get_signal_level(p->agc);
                if (fabsf(sample) > current_est) {
                    agc_rrrf_set_bandwidth(p->agc, p->agc_attack_bw);
                } else {
                    agc_rrrf_set_bandwidth(p->agc, p->agc_decay_bw);
                }
                agc_rrrf_execute(p->agc, sample, &sample);

                // C. Manual Gain
                sample *= p->manual_gain;

                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;

                int16_t pcm = (int16_t)(sample * pcm_scale);
                p->interleaved_pcm[2*i]     = pcm;
                p->interleaved_pcm[2*i + 1] = pcm;
            }

            ring_buffer_write(p->audio_ring_buffer, p->interleaved_pcm, num_resampled * 2 * sizeof(int16_t));
        }

        if (!queue_enqueue(res->pipeline.free_sample_chunk_queue, item)) break;
    }

cleanup:
    log_debug("AM writer thread exiting.");
    return NULL;
}

static void am_finalize(ModuleContext* ctx) {
    AppContext* res = ctx->app;
    if (!res->modules.output_private_data) return;
    AmContext* p = (AmContext*)res->modules.output_private_data;

    if (p->audio_device_initialized) ma_device_uninit(&p->audio_device);
    if (p->audio_ring_buffer) ring_buffer_destroy(p->audio_ring_buffer);

    // No demod to destroy
    if (p->pll) nco_crcf_destroy(p->pll);
    if (p->agc) agc_rrrf_destroy(p->agc);
    if (p->audio_lpf) firfilt_rrrf_destroy(p->audio_lpf);
    if (p->resamp_out) msresamp_rrrf_destroy(p->resamp_out);
}

static void am_get_summary(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "AM Audio");
    add_summary_item(info, "Mode", "%s", s_am_config.force_envelope ? "Envelope (Mag)" : "Synchronous (PLL)");
    add_summary_item(info, "Audio Rate", "%d Hz", AUDIO_SAMPLE_RATE);
    add_summary_item(info, "Filter Cutoff", "%.0f Hz", s_am_config.audio_cutoff);
}

static const struct argparse_option am_cli_options[] = {
    OPT_GROUP("AM Output Options"),
    OPT_FLOAT(0, "am-gain", &s_am_config.gain_val, "Set audio output gain (linear).", NULL, 0, 0),
    OPT_FLOAT(0, "am-cutoff", &s_am_config.audio_cutoff, "Set audio lowpass filter cutoff in Hz (default: 10000).", NULL, 0, 0),
    OPT_BOOLEAN(0, "am-envelope", &s_am_config.force_envelope, "Disable Synchronous AM (PLL) and use Magnitude Envelope Detection.", NULL, 0, 0),
};

const struct argparse_option* am_get_cli_options(int* count) {
    *count = sizeof(am_cli_options) / sizeof(am_cli_options[0]);
    return am_cli_options;
}

static OutputModuleInterface am_api = {
    .initialize = am_initialize,
    .run_writer = am_run_writer,
    .finalize_output = am_finalize,
    .get_summary_info = am_get_summary,
    .validate_options = am_validate_options,
    .get_cli_options = am_get_cli_options,
    .write_chunk = NULL
};

OutputModuleInterface* get_am_output_module_api(void) {
    return &am_api;
}
