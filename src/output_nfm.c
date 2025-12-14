#include "output_nfm.h"
#include "miniaudio.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "ring_buffer.h"
#include "utils.h"
#include "signal_handler.h"
#include "sample_convert.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// --- Liquid DSP ---
#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

// --- Constants ---
#define NFM_AUDIO_RATE          48000
#define NFM_AUDIO_CHANNELS      2       // Output is Stereo (Mono duplicated)
#define NFM_BUFFER_SIZE         (128 * 1024)

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
    ma_device audio_device;
    RingBuffer* audio_ring_buffer;
    bool audio_device_initialized;

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
    float squelch_db;        // Threshold in dB
    int squelch_disabled;    // 1 = disabled (Audio always open)

    int is_narrow;           // 1 = Use 2.5kHz deviation (True NFM)
    int disable_discriminator_filter; // 1 = Disable De-emphasis & LPF (Raw/Data Mode)
} s_nfm_config = {
    .gain = 1.0f,
    .squelch_db = -50.0f,
    .squelch_disabled = 0,   // Default: Squelch is ENABLED (0)
    .is_narrow = 0,          // Default to Standard (5kHz dev)
    .disable_discriminator_filter = 0 // Default to Filter Enabled (Voice mode)
};

// --- Miniaudio Callback ---
static void miniaudio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    NfmContext* ctx = (NfmContext*)pDevice->pUserData;
    if (frameCount == 0) return;

    size_t bytes_needed = frameCount * NFM_AUDIO_CHANNELS * sizeof(int16_t);
    size_t available = ring_buffer_get_size(ctx->audio_ring_buffer);

    if (available < bytes_needed) {
        memset(pOutput, 0, bytes_needed);
        return;
    }
    ring_buffer_read(ctx->audio_ring_buffer, pOutput, bytes_needed);
}

// --- Module Interface ---

static bool nfm_validate_options(AppConfig* config) {
    config->output.format = CF32;

    // 1. Default to NFM_DEFAULT_INPUT_RATE (48k) if not specified
    if (config->output_rate.target_rate == 0.0) {
        config->output_rate.target_rate = NFM_DEFAULT_INPUT_RATE;
        config->output_rate.provided = true;
    }

    // 2. Enforce Bounds
    double rate = config->output_rate.target_rate;
    if (rate < NFM_MIN_INPUT_RATE || rate > NFM_MAX_INPUT_RATE) {
        log_fatal("NFM: Invalid input rate %.0f Hz.", rate);
        log_fatal("Valid range is %.0f Hz to %.0f Hz.", NFM_MIN_INPUT_RATE, NFM_MAX_INPUT_RATE);
        return false;
    }

    return true;
}

static bool nfm_initialize(ModuleContext* ctx) {
    AppResources* res = ctx->resources;
    NfmContext* p = (NfmContext*)mem_arena_alloc(&res->setup_arena, sizeof(NfmContext), true);
    res->output_module_private_data = p;

    // 1. Audio Buffer
    p->audio_ring_buffer = ring_buffer_create(NFM_BUFFER_SIZE);

    // 2. Miniaudio
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_s16;
    deviceConfig.playback.channels = NFM_AUDIO_CHANNELS;
    deviceConfig.sampleRate        = NFM_AUDIO_RATE;
    deviceConfig.dataCallback      = miniaudio_data_callback;
    deviceConfig.pUserData         = p;

    if (ma_device_init(NULL, &deviceConfig, &p->audio_device) != MA_SUCCESS) {
        log_fatal("NFM: Audio init failed.");
        return false;
    }
    p->audio_device_initialized = true;

    // 3. DSP Setup
    p->input_samplerate = (float)ctx->config->output_rate.target_rate;
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
    p->resampler = msresamp_rrrf_create(p->output_ratio, 60.0f);

    // Squelch state
    p->squelch_open = false;
    p->squelch_hang_counter = 0;
    p->squelch_hang_max = (int)(p->input_samplerate * (SQ_HANG_TIME_MS / 1000.0f));

    log_info("NFM: Mode: %s | Squelch: %.0f dB | Discriminator Filter: %s",
             s_nfm_config.is_narrow ? "Narrow (2.5k)" : "Standard (5k)",
             s_nfm_config.squelch_db,
             s_nfm_config.disable_discriminator_filter ? "Disabled (Raw)" : "Enabled (Voice)");

    // 4. Buffers
    size_t in_samples = res->pipeline_alloc_size_samples;
    size_t out_samples = (size_t)ceil(in_samples * p->output_ratio) + 64;

    p->mono_buffer   = mem_arena_alloc(&res->setup_arena, in_samples * sizeof(float), false);
    p->resamp_buffer = mem_arena_alloc(&res->setup_arena, out_samples * sizeof(float), false);
    p->pcm_out       = mem_arena_alloc(&res->setup_arena, out_samples * 2 * sizeof(int16_t), false);

    if (ma_device_start(&p->audio_device) != MA_SUCCESS) return false;

    return true;
}

static void* nfm_run_writer(ModuleContext* ctx) {
    AppResources* res = ctx->resources;
    NfmContext* p = (NfmContext*)res->output_module_private_data;

    // Use a threshold of 80% to trigger backpressure
    const size_t THROTTLE_THRESHOLD = (size_t)(NFM_BUFFER_SIZE * 0.8);

    // Stats Accumulators
    size_t stat_counter = 0;
    size_t stat_rate_threshold = (size_t)(p->input_samplerate * 1.0f);
    double accum_mag_sum = 0.0;
    double accum_mag_sq_sum = 0.0;

    while (true) {
        // --- 1. BACKPRESSURE: Check Audio Buffer BEFORE Dequeue ---
        if (p->audio_ring_buffer) {
            while (ring_buffer_get_size(p->audio_ring_buffer) > THROTTLE_THRESHOLD) {
                #ifdef _WIN32
                    Sleep(10);
                #else
                    usleep(10000);
                #endif

                if (is_shutdown_requested()) {
                    goto cleanup;
                }
            }
        }

        SampleChunk* item = (SampleChunk*)queue_dequeue(res->writer_input_queue);
        if (!item) break;

        if (item->is_last_chunk) {
            utils_wait_for_ring_buffer_drain(p->audio_ring_buffer, 10, 200, 200);
            queue_enqueue(res->free_sample_chunk_queue, item);
            break;
        }

        if (item->frames_to_write > 0) {
            unsigned int n = item->frames_to_write;
            liquid_float_complex* iq = (liquid_float_complex*)item->final_output_data;

            // 1. Calculate Stats (RSSI & SNR)
            float block_power_sum = 0.0f;
            for(unsigned int i=0; i<n; i++) {
                float mag2 = crealf(iq[i])*crealf(iq[i]) + cimagf(iq[i])*cimagf(iq[i]);
                float mag = sqrtf(mag2);
                block_power_sum += mag2;
                accum_mag_sum += mag;
                accum_mag_sq_sum += mag2;
            }
            stat_counter += n;

            // 2. Calculate Block RSSI for Squelch
            float avg_block_power = block_power_sum / (float)n;
            float rssi_db_current = 10.0f * log10f(avg_block_power + 1e-12f);

            // 3. Update Squelch Logic
            // If Squelch is NOT disabled (so it is enabled), run the logic
            if (!s_nfm_config.squelch_disabled) {
                if (rssi_db_current > s_nfm_config.squelch_db) {
                    p->squelch_open = true;
                    p->squelch_hang_counter = p->squelch_hang_max;
                } else {
                    if (p->squelch_hang_counter > 0) {
                        p->squelch_hang_counter -= n;
                    } else {
                        if (rssi_db_current < (s_nfm_config.squelch_db - SQ_HYSTERESIS_DB)) {
                            p->squelch_open = false;
                        }
                    }
                }
            } else {
                // Squelch is disabled, force open
                p->squelch_open = true;
            }

            // 4. Print Statistics
            if (stat_counter >= stat_rate_threshold) {
                double avg_power = accum_mag_sq_sum / (double)stat_counter;
                float rssi_db = 10.0f * log10f((float)avg_power + 1e-12f);

                // Only calculate/display SNR if the squelch is OPEN.
                if (p->squelch_open) {
                    double mean_mag = accum_mag_sum / (double)stat_counter;
                    double signal_pwr = mean_mag * mean_mag;
                    double noise_pwr = avg_power - signal_pwr;
                    if (noise_pwr < 1e-12) noise_pwr = 1e-12;
                    float snr_db = 10.0f * log10f((float)(signal_pwr / noise_pwr));

                    log_info("RSSI: %5.1f dB | SNR: %4.1f dB | Squelch: OPEN  ",
                             rssi_db, snr_db);
                } else {
                    // Squelch is closed: SNR is meaningless/impossible to determine.
                    log_info("RSSI: %5.1f dB | Squelch: CLOSED",
                             rssi_db);
                }

                stat_counter = 0;
                accum_mag_sum = 0.0;
                accum_mag_sq_sum = 0.0;
            }

            // 5. Demodulate I/Q -> Mono Audio
            freqdem_demodulate_block(p->fm_demod, iq, n, p->mono_buffer);

            // 6. Audio Processing Chain
            for(unsigned int i=0; i<n; i++) {
                float sample = p->mono_buffer[i];

                // If FILTER IS ENABLED (Normal Voice Operation):
                // 1. Apply De-emphasis (Correction for FM audio)
                // 2. Apply LPF (Cutoff > 4kHz to remove hiss)
                //
                // If FILTER IS DISABLED (Raw / Discriminator Mode):
                // We SKIP BOTH to preserve the full bandwidth for digital decoders (POCSAG, DMR, etc).

                if (!s_nfm_config.disable_discriminator_filter) {
                    iirfilt_rrrf_execute(p->deemph_filter, sample, &sample);
                    iirfilt_rrrf_execute(p->audio_lpf, sample, &sample);
                }

                // C. Gain
                sample *= s_nfm_config.gain;

                // D. Squelch Gate
                if (!p->squelch_open) {
                    sample = 0.0f;
                }

                p->mono_buffer[i] = sample;
            }

            // 7. Resample to 48k
            unsigned int num_resampled;
            msresamp_rrrf_execute(p->resampler, p->mono_buffer, n, p->resamp_buffer, &num_resampled);

            // 8. Convert to S16 Stereo (Duplicate Mono)
            for(unsigned int i=0; i<num_resampled; i++) {
                float s = p->resamp_buffer[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;

                int16_t pcm = (int16_t)(s * 32767.0f);
                p->pcm_out[2*i] = pcm;     // Left
                p->pcm_out[2*i+1] = pcm;   // Right
            }

            ring_buffer_write(p->audio_ring_buffer, p->pcm_out, num_resampled * 2 * sizeof(int16_t));
        }

        queue_enqueue(res->free_sample_chunk_queue, item);
    }

cleanup:
    log_debug("NFM writer thread exiting.");
    return NULL;
}

static void nfm_finalize(ModuleContext* ctx) {
    AppResources* res = ctx->resources;
    if (!res->output_module_private_data) return;
    NfmContext* p = (NfmContext*)res->output_module_private_data;

    if (p->audio_device_initialized) ma_device_uninit(&p->audio_device);
    if (p->audio_ring_buffer) ring_buffer_destroy(p->audio_ring_buffer);
    if (p->fm_demod) freqdem_destroy(p->fm_demod);
    if (p->deemph_filter) iirfilt_rrrf_destroy(p->deemph_filter);
    if (p->audio_lpf) iirfilt_rrrf_destroy(p->audio_lpf);
    if (p->resampler) msresamp_rrrf_destroy(p->resampler);
}

static void nfm_get_summary(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    const char* mode = s_nfm_config.is_narrow ? "Narrow (2.5k Dev)" : "Standard (5k Dev)";
    // Fixed typo here: Eiscriminator -> Discriminator
    const char* type = s_nfm_config.disable_discriminator_filter ? "Raw (Discriminator Filter Disabled)" : "Voice (Discriminator Filter Enabled)";

    add_summary_item(info, "Output Type", "NFM");
    add_summary_item(info, "Mode", "%s", mode);
    add_summary_item(info, "Audio", "%s", type);
    add_summary_item(info, "Squelch", "%.0f dB", s_nfm_config.squelch_db);
}

static const struct argparse_option nfm_cli_options[] = {
    OPT_GROUP("NFM Output Options"),
    OPT_FLOAT(0, "nfm-gain", &s_nfm_config.gain, "Audio gain (default: 1.0)", NULL, 0, 0),
    OPT_FLOAT(0, "nfm-squelch", &s_nfm_config.squelch_db, "Squelch threshold in dB (default: -50.0)", NULL, 0, 0),
    OPT_BOOLEAN(0, "nfm-narrow", &s_nfm_config.is_narrow, "Enable Narrow mode (2.5kHz dev). Default is Standard (5kHz dev).", NULL, 0, 0),
    OPT_BOOLEAN(0, "nfm-no-squelch", &s_nfm_config.squelch_disabled, "Disable squelch (force open audio)", NULL, 0, 0),
    OPT_BOOLEAN(0, "nfm-no-discriminator-filter", &s_nfm_config.disable_discriminator_filter, "Disable the discriminator filter.", NULL, 0, 0),
};

const struct argparse_option* nfm_get_cli_options(int* count) {
    *count = sizeof(nfm_cli_options) / sizeof(nfm_cli_options[0]);
    return nfm_cli_options;
}

static OutputModuleInterface nfm_api = {
    .initialize = nfm_initialize,
    .run_writer = nfm_run_writer,
    .finalize_output = nfm_finalize,
    .get_summary_info = nfm_get_summary,
    .validate_options = nfm_validate_options,
    .get_cli_options = nfm_get_cli_options,
    .write_chunk = NULL
};

OutputModuleInterface* get_nfm_output_module_api(void) {
    return &nfm_api;
}
