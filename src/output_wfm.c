/*
 * Original Work Copyright (c) 2017-2022 windytan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * -----------------------------------------------------------------------------
 *
 * Modifications Copyright (C) 2025 iq_tool
 *
 * The modifications to this file are licensed under the GNU General Public License v3.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "output_wfm.h"
#include "miniaudio.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "utils.h"
#include "signal_handler.h"
#include "queue.h"
#include "sample_convert.h"
#include "redsea_wrapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <liquid.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Constants from demux.h ---
#define WFM_BUFFER_SIZE         8192
#define WFM_PILOT_HZ            19000.0f
#define WFM_PLL_BW_HZ           10.0f
#define WFM_PILOT_FIR_USEC      740.0f
#define WFM_PILOT_FIR_HALFBAND  800.0f
#define WFM_AUDIO_FIR_CUTOFF    16500.0f
#define WFM_AUDIO_FIR_LEN_USEC  740.0f
#define WFM_DEEMPH_ORDER        1
#define WFM_STEREO_SEPARATION   1.2f

// --- Logging Config ---
#define WFM_STATS_INTERVAL_SEC  1.0f

// --- Gain Staging ---
// Scaling factor applied to the MPX signal after demodulation.
// Standard FM deviation (75kHz) maps to 1.0 in freqdem.
// We scale this down (~ -16.5 dB) to create headroom for the Pilot tone,
// Stereo Matrix (Sum + Diff), and De-emphasis filters.
#define WFM_MPX_SCALING_FACTOR  0.15f

// --- Audio Config ---
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_CHANNELS          2
#define AUDIO_BUFFER_SIZE       (512 * 1024)

// --- MPX Rate Constraints ---
#define WFM_MIN_MPX_RATE        192000.0
#define WFM_MAX_MPX_RATE        384000.0
#define WFM_DEFAULT_MPX_RATE    240000.0

// --- Internal Structures ---

typedef struct {
    float buffer[WFM_BUFFER_SIZE];
    float sum;
    int idx;
} RunningAverage;

typedef struct {
    float coeff_B[3 * ((WFM_DEEMPH_ORDER + 1) / 2)];
    float coeff_A[3 * ((WFM_DEEMPH_ORDER + 1) / 2)];
    iirfilt_rrrf iir_l;
    iirfilt_rrrf iir_r;
} DeEmphasis;

typedef struct {
    // Pipeline State
    ma_device audio_device;
    RingBuffer* audio_ring_buffer;
    bool audio_device_initialized;

    // DSP Objects (Liquid)
    freqdem fm_demod;           // I/Q -> MPX

    nco_crcf nco_pilot_approx;
    nco_crcf nco_pilot_exact;
    nco_crcf nco_stereo_subcarrier;

    firfilt_crcf fir_pilot;     // Complex FIR
    firfilt_rrrf fir_sum;       // Real FIR
    firfilt_rrrf fir_diff;      // Real FIR

    DeEmphasis deemphasis;
    RunningAverage pilotnoise;

    // Output Resamplers (MPX Rate -> 48k)
    msresamp_rrrf resamp_out_l;
    msresamp_rrrf resamp_out_r;
    float output_resample_ratio;

    // Processing State
    float input_samplerate;
    float gain;

    // Scratch Buffers
    float* mpx_buffer;
    float* audio_out_l;
    float* audio_out_r;
    int16_t* interleaved_pcm;

    // Optional Buffer for Raw MPX S16 conversion
    int16_t* mpx_s16_buffer;

    // RDS
    RedseaHandle redsea;
    RdsState last_rds_state;
    size_t rds_display_counter;
    size_t rds_display_threshold;
} WfmContext;

// --- CLI Config ---
static struct {
    float deemph_us;
    float gain_val;
    int force_stereo;
    int force_mono;
    int raw_mpx_stdout;
#ifdef WITH_REDSEA
    int rds_disable; // Default 0 (Enabled)
    int rds_us_mode;
    int rds_partial; // Default 0 (Disabled)
#endif
} s_wfm_config = {
    .deemph_us = 75.0f,
    .gain_val = 5.0f,
    .force_stereo = 0,
    .force_mono = 0,
    .raw_mpx_stdout = 0,
#ifdef WITH_REDSEA
    .rds_disable = 0, // Enabled by default
    .rds_us_mode = 0,
    .rds_partial = 0
#endif
};

// --- Helpers ---

static float angular_freq(float hertz, float sample_rate) {
    return hertz * 2.0f * (float)M_PI / sample_rate;
}

static void running_average_init(RunningAverage* ra) {
    memset(ra->buffer, 0, sizeof(ra->buffer));
    ra->sum = 0.0f;
    ra->idx = 0;
    // Pre-fill as done in demux.cpp
    for (int i = 0; i < WFM_BUFFER_SIZE; i++) {
        ra->sum -= ra->buffer[ra->idx];
        ra->buffer[ra->idx] = 9.0f;
        ra->sum += ra->buffer[ra->idx];
        ra->idx = (ra->idx + 1) % WFM_BUFFER_SIZE;
    }
}

static void running_average_push(RunningAverage* ra, float val) {
    ra->sum -= ra->buffer[ra->idx];
    ra->buffer[ra->idx] = val;
    ra->sum += ra->buffer[ra->idx];
    ra->idx = (ra->idx + 1) % WFM_BUFFER_SIZE;
}

static float running_average_get(RunningAverage* ra) {
    return ra->sum / (float)WFM_BUFFER_SIZE;
}

static void deemphasis_init(DeEmphasis* de, float time_constant_us, float sample_rate) {
    float cutoff = (1.0f / (2.0f * (float)M_PI * time_constant_us * 1e-6f)) / sample_rate;

    liquid_iirdes(LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS,
                  WFM_DEEMPH_ORDER, cutoff, 0.0f, 10.0f, 10.0f,
                  de->coeff_B, de->coeff_A);

    int num_sections = (WFM_DEEMPH_ORDER + 1) / 2;
    de->iir_l = iirfilt_rrrf_create_sos(de->coeff_B, de->coeff_A, num_sections);
    de->iir_r = iirfilt_rrrf_create_sos(de->coeff_B, de->coeff_A, num_sections);
}

static void deemphasis_execute(DeEmphasis* de, float in_l, float in_r, float* out_l, float* out_r) {
    iirfilt_rrrf_execute(de->iir_l, in_l, out_l);
    iirfilt_rrrf_execute(de->iir_r, in_r, out_r);
}

static void deemphasis_destroy(DeEmphasis* de) {
    if (de->iir_l) iirfilt_rrrf_destroy(de->iir_l);
    if (de->iir_r) iirfilt_rrrf_destroy(de->iir_r);
}

// --- Miniaudio Callback ---
static void miniaudio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    WfmContext* ctx = (WfmContext*)pDevice->pUserData;
    if (frameCount == 0) return;

    size_t bytes_needed = frameCount * AUDIO_CHANNELS * sizeof(int16_t);
    size_t available = ring_buffer_get_size(ctx->audio_ring_buffer);

    if (available < bytes_needed) {
        memset(pOutput, 0, bytes_needed); // Underrun: Silence
        return;
    }
    ring_buffer_read(ctx->audio_ring_buffer, pOutput, bytes_needed);
}

// --- Module Interface Implementation ---

static bool wfm_output_validate_options(AppConfig* config) {
    // 1. Force Pipeline Format to CF32
    // We need high-precision float I/Q for the FM demodulator.
    config->output.format = CF32; 

    // 2. Validate/Enforce MPX Rate
    if (config->output_rate.target_rate == 0.0) {
        // Case A: User did not specify --output-rate.
        // Force the default MPX rate.
        config->output_rate.target_rate = WFM_DEFAULT_MPX_RATE;
        config->output_rate.provided = true; // Mark as provided so setup.c respects it
        log_info("WFM: No output rate specified. Defaulting to MPX rate %.0f Hz.", WFM_DEFAULT_MPX_RATE);
    } else {
        // Case B: User specified a rate. Validate range.
        double rate = config->output_rate.target_rate;
        if (rate < WFM_MIN_MPX_RATE || rate > WFM_MAX_MPX_RATE) {
            log_fatal("WFM: Invalid MPX rate %.0f Hz.", rate);
            log_fatal("Valid range is %.0f Hz to %.0f Hz.", WFM_MIN_MPX_RATE, WFM_MAX_MPX_RATE);
            return false;
        }
    }

    // 3. Check for conflicting forced modes
    if (s_wfm_config.force_stereo && s_wfm_config.force_mono) {
        log_fatal("WFM: Cannot force both Stereo and Mono simultaneously. Please choose one.");
        return false;
    }

    return true;
}

static bool wfm_output_initialize(ModuleContext* ctx) {
    AppContext* res = ctx->app;

    // Windows: stdout defaults to text mode (\n -> \r\n), which corrupts binary I/Q data.
    // We must forcefully set it to binary if the user requested raw output.
    if (s_wfm_config.raw_mpx_stdout) {
        #ifdef _WIN32
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            log_error("WFM: Failed to set stdout to binary mode.");
            return false;
        }
        #endif
    }

    WfmContext* p = (WfmContext*)mem_arena_alloc(&res->pipeline.setup_arena, sizeof(WfmContext), true);
    if (!p) return false;
    res->module.output_private_data = p;

    // 1. Setup Audio Ring Buffer
    p->audio_ring_buffer = ring_buffer_create(AUDIO_BUFFER_SIZE);
    if (p->audio_ring_buffer) {
        double bytes_per_sec = (double)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * sizeof(int16_t);
        double duration = (double)AUDIO_BUFFER_SIZE / bytes_per_sec;
        log_info("WFM: Audio Ring Buffer created: %zu bytes (%.2f seconds)", (size_t)AUDIO_BUFFER_SIZE, duration);
    }
    if (!p->audio_ring_buffer) return false;

    // 2. Setup Miniaudio
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_s16;
    deviceConfig.playback.channels = AUDIO_CHANNELS;
    deviceConfig.sampleRate        = AUDIO_SAMPLE_RATE;
    deviceConfig.dataCallback      = miniaudio_data_callback;
    deviceConfig.pUserData         = p;

    if (ma_device_init(NULL, &deviceConfig, &p->audio_device) != MA_SUCCESS) {
        log_fatal("WFM: Failed to initialize audio device.");
        return false;
    }
    p->audio_device_initialized = true;

    // 3. DSP Configuration
    float mpx_rate = (float)ctx->config->output_rate.target_rate;
    p->input_samplerate = mpx_rate;
    p->gain = s_wfm_config.gain_val;

    log_info("WFM: Configuring DSP for MPX Rate: %.0f Hz -> Audio: %d Hz (De-emphasis: %.0fus)",
             mpx_rate, AUDIO_SAMPLE_RATE, s_wfm_config.deemph_us);

    // 4. Create Liquid Objects

    float deviation = 75000.0f;
    // Engineering Note: We use the correct modulation index math here (deviation / rate).
    // This results in a "hot" signal (75kHz = 1.0), so we apply WFM_MPX_SCALING_FACTOR
    // in the writer loop to create headroom for stereo processing.
    float kf = deviation / mpx_rate;
    p->fm_demod = freqdem_create(kf);

    p->nco_pilot_approx = nco_crcf_create(LIQUID_VCO);
    nco_crcf_set_frequency(p->nco_pilot_approx, angular_freq(WFM_PILOT_HZ, mpx_rate));

    p->nco_pilot_exact = nco_crcf_create(LIQUID_VCO);
    nco_crcf_set_frequency(p->nco_pilot_exact, angular_freq(WFM_PILOT_HZ, mpx_rate));
    nco_crcf_pll_set_bandwidth(p->nco_pilot_exact, WFM_PLL_BW_HZ / mpx_rate);

    p->nco_stereo_subcarrier = nco_crcf_create(LIQUID_VCO);
    nco_crcf_set_frequency(p->nco_stereo_subcarrier, 2.0f * angular_freq(WFM_PILOT_HZ, mpx_rate));

    // Filters
    int pilot_fir_half_len = (int)(mpx_rate * 1e-6f * WFM_PILOT_FIR_USEC);
    int pilot_fir_len = pilot_fir_half_len * 2 + 1;
    p->fir_pilot = firfilt_crcf_create_kaiser(pilot_fir_len, WFM_PILOT_FIR_HALFBAND / mpx_rate, 60.0f, 0.0f);
    firfilt_crcf_set_scale(p->fir_pilot, 2.0f * (WFM_PILOT_FIR_HALFBAND / mpx_rate));

    int audio_fir_len = (int)(WFM_AUDIO_FIR_LEN_USEC * 1e-6f * mpx_rate);
    if (audio_fir_len % 2 == 0) audio_fir_len++;
    float audio_fc = WFM_AUDIO_FIR_CUTOFF / mpx_rate;

    p->fir_sum = firfilt_rrrf_create_kaiser(audio_fir_len, audio_fc, 60.0f, 0.0f);
    firfilt_rrrf_set_scale(p->fir_sum, 2.0f * audio_fc);

    p->fir_diff = firfilt_rrrf_create_kaiser(audio_fir_len, audio_fc, 60.0f, 0.0f);
    firfilt_rrrf_set_scale(p->fir_diff, 2.0f * audio_fc);

    deemphasis_init(&p->deemphasis, s_wfm_config.deemph_us, mpx_rate);
    running_average_init(&p->pilotnoise);

    // Output Resamplers
    p->output_resample_ratio = (float)AUDIO_SAMPLE_RATE / mpx_rate;
    p->resamp_out_l = msresamp_rrrf_create(p->output_resample_ratio, 60.0f);
    p->resamp_out_r = msresamp_rrrf_create(p->output_resample_ratio, 60.0f);

    // 5. Scratch Buffers (Local Elastic Allocation)
    // We calculate the maximum buffer size required for ANY stage of this specific module.
    // If upsampling (e.g. 32k -> 48k), the output buffer needs more space than the input.
    size_t buf_samples = res->pipeline.alloc_size_samples;
    size_t out_buf_samples = (size_t)ceil(buf_samples * p->output_resample_ratio) + 128;
    size_t max_dsp_samples = (buf_samples > out_buf_samples) ? buf_samples : out_buf_samples;

    p->mpx_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    p->audio_out_l = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    p->audio_out_r = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);

    // Interleaved buffer is always sized for the output
    p->interleaved_pcm = mem_arena_alloc(&res->pipeline.setup_arena, out_buf_samples * 2 * sizeof(int16_t), false);

    if (!p->mpx_buffer || !p->audio_out_l || !p->audio_out_r || !p->interleaved_pcm) return false;

    // Optional: Allocate S16 buffer for MPX stdout if requested
    if (s_wfm_config.raw_mpx_stdout) {
        p->mpx_s16_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(int16_t), false);
        if (!p->mpx_s16_buffer) return false;
    }

#ifdef WITH_REDSEA
    // 6. Initialize RDS (Enabled unless disabled)
    if (!s_wfm_config.rds_disable) {
        p->redsea = redsea_init(p->input_samplerate, s_wfm_config.rds_us_mode, s_wfm_config.rds_partial);
        memset(&p->last_rds_state, 0, sizeof(RdsState));

        p->rds_display_counter = 0;
        p->rds_display_threshold = (size_t)(p->input_samplerate * 1.0); // 1 second

        log_info("WFM: RDS Decoder enabled (Mode: %s%s)",
                 s_wfm_config.rds_us_mode ? "RBDS/US" : "RDS/World",
                 s_wfm_config.rds_partial ? ", Partial Text" : "");
    } else {
        p->redsea = NULL;
    }
#endif
    // 7. Start Audio
    if (ma_device_start(&p->audio_device) != MA_SUCCESS) {
        log_error("WFM: Failed to start audio callback.");
        return false;
    }

    return true;
}

static void* wfm_output_run_writer(ModuleContext* ctx) {
    AppContext* res = ctx->app;
    WfmContext* p = (WfmContext*)res->module.output_private_data;

    // Throttle if buffer is > 80% full
    const size_t THROTTLE_THRESHOLD = (size_t)(AUDIO_BUFFER_SIZE * 0.8);

    // --- Statistics Accumulators ---
    size_t stat_counter = 0;
    // Log once per second (Signal Time)
    size_t stat_rate_threshold = (size_t)(p->input_samplerate * WFM_STATS_INTERVAL_SEC);

    // Using double for accumulators to prevent overflow
    double accum_mag_sum = 0.0;       // For Mean Magnitude (SNR)
    double accum_mag_sq_sum = 0.0;    // For Total Power (RSSI)
    double accum_pilot_mag_sum = 0.0; // For Pilot Presence detection

    // Average accumulators for Stereo/Pilot stats
    double accum_stereo_pct_sum = 0.0;
    double accum_pilot_err_sq_sum = 0.0;
    size_t accum_pilot_count = 0;

    while (true) {
        // --- 1. BACKPRESSURE: Check Audio Buffer BEFORE Dequeue ---
        if (p->audio_ring_buffer) {
            // While the audio buffer is too full, we sleep.
            // This leaves the SampleChunk in the upstream queue, preserving app.
            ring_buffer_wait_for_threshold(p->audio_ring_buffer, THROTTLE_THRESHOLD);
            if (is_shutdown_requested()) goto cleanup;
        }

        // --- 2. Acquire Data ---
        SampleChunk* item = (SampleChunk*)queue_dequeue(res->pipeline.writer_input_queue);
        if (!item) break;

        if (item->stream_discontinuity_event) {
            queue_enqueue(res->pipeline.free_sample_chunk_queue, item);
            continue;
        }

        // --- END OF STREAM HANDLING ---
        if (item->is_last_chunk) {

            // Wait for the ring buffer to drain using the reusable utility.
            // Poll every 10ms, timeout if stalled for 200ms, wait 200ms for hardware padding.
            utils_wait_for_ring_buffer_drain(p->audio_ring_buffer, 10, 200, 200);

            queue_enqueue(res->pipeline.free_sample_chunk_queue, item);
            break;
        }

        if (item->frames_to_write > 0) {
            ComplexFloat* iq_in = (ComplexFloat*)item->final_output_data;
            unsigned int num_frames = item->frames_to_write;

            // --- STATS PRE-CALCULATION (Raw I/Q) ---
            // Fix: Cast to liquid type to ensure compatibility with cabsf()
            liquid_float_complex* iq_ptr = (liquid_float_complex*)iq_in;

            // Calculate RSSI and SNR estimates based on Input I/Q Magnitude
            for (unsigned int k = 0; k < num_frames; k++) {
                // Calculate magnitude of complex sample: sqrt(I^2 + Q^2)
                float mag = cabsf(iq_ptr[k]);
                accum_mag_sum += mag;
                accum_mag_sq_sum += (mag * mag);
            }
            // Count frames towards the 1-second interval
            stat_counter += num_frames;

            // --- STAGE 1: I/Q to MPX ---
            freqdem_demodulate_block(p->fm_demod,
                                     iq_ptr,
                                     num_frames,
                                     p->mpx_buffer);

            // --- STAGE 1.5: Optional Raw MPX Output (S16 Mono) ---
            if (s_wfm_config.raw_mpx_stdout && p->mpx_s16_buffer) {
                for (unsigned int k = 0; k < num_frames; k++) {
                    // Scale float MPX (-1.0 to 1.0 approx) to S16 range
                    float sample = p->mpx_buffer[k] * 32767.0f;

                    // Hard Clamp
                    if (sample > 32767.0f) sample = 32767.0f;
                    if (sample < -32768.0f) sample = -32768.0f;

                    p->mpx_s16_buffer[k] = (int16_t)sample;
                }
                fwrite(p->mpx_s16_buffer, sizeof(int16_t), num_frames, stdout);
            }

#ifdef WITH_REDSEA
            // --- STAGE 1.6: RDS Decoding ---
            if (p->redsea) {
                RdsState current;
                redsea_process_mpx(p->redsea, p->mpx_buffer, num_frames, &current);

                // Increment counter
                p->rds_display_counter += num_frames;

                // Display every 1 second if valid data exists
                if (p->rds_display_counter >= p->rds_display_threshold && current.valid) {

                    // Reset counter
                    p->rds_display_counter = 0;

                    // Trim RadioText
                    char clean_rt[65];
                    strncpy(clean_rt, current.radiotext, 64);
                    clean_rt[64] = '\0';
                    size_t rt_len = strlen(clean_rt);
                    while (rt_len > 0 && clean_rt[rt_len - 1] == ' ') {
                        clean_rt[rt_len - 1] = '\0';
                        rt_len--;
                    }

                    // Log Output - Always display
                    if (s_wfm_config.rds_us_mode && current.callsign[0] != '\0') {
                        log_info("RBDS PI: %04X | CALL: %s | PS: %s | PTY: %s | PTYN: %s | RT: %s | TP: %d | TA: %d | MS: %d | ST: %d | CMP: %d | DYN: %d", 
                                 current.pi_code,
                                 current.callsign,
                                 current.ps_name,
                                 current.program_type,
                                 current.pty_name,
                                 clean_rt,
                                 current.tp ? 1 : 0,
                                 current.ta ? 1 : 0,
                                 current.is_music ? 1 : 0,
                                 current.stereo ? 1 : 0,
                                 current.compressed ? 1 : 0,
                                 current.dynamic ? 1 : 0);
                    } else {
                        log_info("RDS PI: %04X | PS: %s | PTY: %s | PTYN: %s | RT: %s | TP: %d | TA: %d | MS: %d | ST: %d | CMP: %d | DYN: %d", 
                                 current.pi_code,
                                 current.ps_name,
                                 current.program_type,
                                 current.pty_name,
                                 clean_rt,
                                 current.tp ? 1 : 0,
                                 current.ta ? 1 : 0,
                                 current.is_music ? 1 : 0,
                                 current.stereo ? 1 : 0,
                                 current.compressed ? 1 : 0,
                                 current.dynamic ? 1 : 0);
                    }

                    // Clock Time display (if present and changed)
                    if (current.clock_time[0] != '\0' && strcmp(current.clock_time, p->last_rds_state.clock_time) != 0) {
                        log_info("RDS/RBDS CT: %s", current.clock_time);
                    }

                    p->last_rds_state = current;
                }
            }
#endif
            // --- STAGE 2: Stereo Decoding & Anti-Alias ---
            for (unsigned int i = 0; i < num_frames; i++) {
                // ENGINEERING FIX: Explicit Gain Staging
                // Scale down the hot MPX signal to create headroom for stereo processing.
                float insample = p->mpx_buffer[i] * WFM_MPX_SCALING_FACTOR;

                // A. Pilot Bandpass
                liquid_float_complex pilot_mix_down;
                nco_crcf_mix_down(p->nco_pilot_approx, insample + 0.0f * I, &pilot_mix_down);

                firfilt_crcf_push(p->fir_pilot, pilot_mix_down);
                liquid_float_complex fir_out;
                firfilt_crcf_execute(p->fir_pilot, &fir_out);

                // Track Pilot Magnitude for Mono Detection
                accum_pilot_mag_sum += cabsf(fir_out);

                liquid_float_complex pilot;
                nco_crcf_mix_up(p->nco_pilot_approx, fir_out, &pilot);
                nco_crcf_step(p->nco_pilot_approx);

                // B. Generate 38kHz Carrier
                float pilot_phase = nco_crcf_get_phase(p->nco_pilot_exact);
                nco_crcf_set_phase(p->nco_stereo_subcarrier, 2.0f * pilot_phase);

                // C. Pilot PLL
                liquid_float_complex pll_val;
                nco_crcf_cexpf(p->nco_pilot_exact, &pll_val);
                float phase_error = cargf(pilot * conjf(pll_val));

                if (i % 4 == 0) {
                    nco_crcf_pll_step(p->nco_pilot_exact, phase_error);

                    // Accumulate stats for logging (independent of control loop)
                    accum_pilot_err_sq_sum += (phase_error * phase_error);
                    accum_pilot_count++;

                    // Push to control loop logic
                    running_average_push(&p->pilotnoise, phase_error * phase_error);
                }
                nco_crcf_step(p->nco_pilot_exact);

                // D. Stereo Gain / Mono Revert Logic
                float stereogain = 0.0f;

                if (s_wfm_config.force_mono) {
                    stereogain = 0.0f;
                } else if (s_wfm_config.force_stereo) {
                    stereogain = 1.0f;
                } else {
                    // Adaptive Mode
                    float avg_noise = running_average_get(&p->pilotnoise);
                    float val = WFM_STEREO_SEPARATION - avg_noise;
                    if (val < 0.0f) val = 0.0f;
                    if (val > 1.0f) val = 1.0f;
                    stereogain = val;
                }

                // Stats: Accumulate Stereo Percentage
                accum_stereo_pct_sum += (stereogain * 100.0f);

                // E. Decode Stereo & Anti-Alias
                firfilt_rrrf_push(p->fir_sum, insample);

                liquid_float_complex sc_mix;
                nco_crcf_mix_down(p->nco_stereo_subcarrier, insample + 0.0f * I, &sc_mix);
                firfilt_rrrf_push(p->fir_diff, cimagf(sc_mix));

                float sum, diff;
                firfilt_rrrf_execute(p->fir_sum, &sum);
                firfilt_rrrf_execute(p->fir_diff, &diff);

                diff = 2.0f * diff * stereogain;

                float left = (sum + diff) * p->gain;
                float right = (sum - diff) * p->gain;

                // F. De-emphasis
                deemphasis_execute(&p->deemphasis, left, right, &left, &right);

                p->audio_out_l[i] = left;
                p->audio_out_r[i] = right;
            }

            // --- STAGE 3: Log Statistics (Every 1 second) ---
            if (stat_counter >= stat_rate_threshold) {

                // 1. Calculate RSSI (Average Power)
                double avg_power = accum_mag_sq_sum / (double)stat_counter;
                float rssi_db = 10.0f * log10f((float)avg_power + 1e-10f);

                // 2. Calculate SNR
                double mean_mag = accum_mag_sum / (double)stat_counter;
                double signal_pwr = mean_mag * mean_mag;
                double noise_pwr = avg_power - signal_pwr;
                if (noise_pwr < 1e-10) noise_pwr = 1e-10;
                float snr_db = 10.0f * log10f((float)(signal_pwr / noise_pwr));

                // 3. Calculate Pilot Error % First
                float avg_pilot_mse = 0.0f;
                if (accum_pilot_count > 0) {
                    avg_pilot_mse = (float)(accum_pilot_err_sq_sum / (double)accum_pilot_count);
                }
                float pilot_pct = sqrtf(avg_pilot_mse) * 100.0f;

                // 4. Detect Mono/Stereo status
                // A. Check Pilot Magnitude (low threshold for weak stations)
                double avg_pilot = accum_pilot_mag_sum / (double)stat_counter;

                // B. Determine Status:
                // - Signal too weak (pilot magnitude < 0.001)
                // - OR Pilot Error > 100% (PLL unlocked / random noise)
                bool is_mono_station = (avg_pilot < 0.001) || (pilot_pct > 100.0f);

                // C. Sanity Check: If Stereo Separation is active (> 1%), force it to valid.
                float avg_stereo_pct = (float)(accum_stereo_pct_sum / (double)stat_counter);
                if (avg_stereo_pct > 1.0f) {
                    is_mono_station = false;
                }

                if (is_mono_station || s_wfm_config.force_mono) {
                     log_info("RSSI: %.1f dB | SNR: %.1f dB | Stereo Separation: Mono | Pilot Phase Error: NA",
                         rssi_db, snr_db);
                } else {
                    log_info("RSSI: %.1f dB | SNR: %.1f dB | Stereo Separation: %.1f%% | Pilot Phase Error: %.1f%%", 
                            rssi_db, snr_db, avg_stereo_pct, pilot_pct);
                }

                // Reset all counters
                stat_counter = 0;
                accum_mag_sum = 0.0;
                accum_mag_sq_sum = 0.0;
                accum_pilot_mag_sum = 0.0;
                accum_stereo_pct_sum = 0.0;
                accum_pilot_err_sq_sum = 0.0;
                accum_pilot_count = 0;
            }

            // --- STAGE 4: Resample to 48kHz ---
            unsigned int num_resampled_l, num_resampled_r;

            // Use mpx_buffer as temporary output for Left (safe re-use due to max_dsp_samples).
            msresamp_rrrf_execute(p->resamp_out_l, p->audio_out_l, num_frames, p->mpx_buffer, &num_resampled_l);

            // Use audio_out_l as temporary output for Right (safe re-use since read is done).
            msresamp_rrrf_execute(p->resamp_out_r, p->audio_out_r, num_frames, p->audio_out_l, &num_resampled_r);

            // --- STAGE 5: Convert to S16 ---
            // Note: Left audio is currently in mpx_buffer, Right is in audio_out_l.
            sample_convert_interleave_f32_to_s16(
                p->mpx_buffer,      // Left Plane
                p->audio_out_l,     // Right Plane
                p->interleaved_pcm, // Destination
                num_resampled_l     // Count
            );

            // --- STAGE 6: Push to Audio Ring Buffer ---
            size_t bytes_to_write = num_resampled_l * 2 * sizeof(int16_t);

            // Note: We do NOT loop/sleep here anymore. We rely on the throttle
            // at the start of the loop to ensure sufficient space exists.
            ring_buffer_write(p->audio_ring_buffer, p->interleaved_pcm, bytes_to_write);
        }

        if (!queue_enqueue(res->pipeline.free_sample_chunk_queue, item)) break;
    }

cleanup:
    log_debug("WFM writer thread exiting.");
    return NULL;
}

static void wfm_output_cleanup(ModuleContext* ctx) {
    AppContext* res = ctx->app;
    if (!res->module.output_private_data) return;
    WfmContext* p = (WfmContext*)res->module.output_private_data;

    if (p->audio_device_initialized) {
        ma_device_uninit(&p->audio_device);
    }
    if (p->audio_ring_buffer) {
        ring_buffer_destroy(p->audio_ring_buffer);
    }

    if (p->fm_demod) freqdem_destroy(p->fm_demod);
    if (p->nco_pilot_approx) nco_crcf_destroy(p->nco_pilot_approx);
    if (p->nco_pilot_exact) nco_crcf_destroy(p->nco_pilot_exact);
    if (p->nco_stereo_subcarrier) nco_crcf_destroy(p->nco_stereo_subcarrier);
    if (p->fir_pilot) firfilt_crcf_destroy(p->fir_pilot);
    if (p->fir_sum) firfilt_rrrf_destroy(p->fir_sum);
    if (p->fir_diff) firfilt_rrrf_destroy(p->fir_diff);
    deemphasis_destroy(&p->deemphasis);
    if (p->resamp_out_l) msresamp_rrrf_destroy(p->resamp_out_l);
    if (p->resamp_out_r) msresamp_rrrf_destroy(p->resamp_out_r);

#ifdef WITH_REDSEA
    if (p->redsea) {
        redsea_free(p->redsea);
        p->redsea = NULL;
    }
#endif
}

static void wfm_output_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "WFM Stereo Audio");
    add_summary_item(info, "Audio Rate", "%d Hz", AUDIO_SAMPLE_RATE);
    add_summary_item(info, "De-emphasis", "%.0f us", s_wfm_config.deemph_us);

    const char* mode = "Adaptive";
    if (s_wfm_config.force_mono) mode = "Forced Mono";
    if (s_wfm_config.force_stereo) mode = "Forced Stereo";
    add_summary_item(info, "Stereo Mode", "%s", mode);
}

static const struct argparse_option wfm_output_cli_options[] = {
    OPT_GROUP("WFM Output (wfm)"),
    OPT_FLOAT(0, "wfm-de-emphasis-time", &s_wfm_config.deemph_us, "Set FM de-emphasis time constant in microseconds (default: 75.0).", NULL, 0, 0),
    OPT_FLOAT(0, "wfm-gain", &s_wfm_config.gain_val, "Set audio output gain (linear).", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-force-stereo", &s_wfm_config.force_stereo, "Force stereo decoding regardless of signal quality.", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-force-mono", &s_wfm_config.force_mono, "Force mono output.", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-raw-mpx-stdout", &s_wfm_config.raw_mpx_stdout, "Pipe raw MPX data (S16 Mono) to stdout while playing audio.", NULL, 0, 0),
#ifdef WITH_REDSEA
    OPT_BOOLEAN(0, "wfm-no-rds", &s_wfm_config.rds_disable, "Disable RDS decoding (Enabled by default).", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-rbds", &s_wfm_config.rds_us_mode, "Enable US RBDS mode (Callsigns + US Program Types).", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-rds-partial", &s_wfm_config.rds_partial, "Show partial/noisy RDS text (PS/RT).", NULL, 0, 0),
#endif
};

const struct argparse_option* wfm_output_get_cli_options(int* count) {
    *count = sizeof(wfm_output_cli_options) / sizeof(wfm_output_cli_options[0]);
    return wfm_output_cli_options;
}

static OutputModuleInterface s_wfm_output_api = {
    .initialize = wfm_output_initialize,
    .run_writer = wfm_output_run_writer,
    .cleanup = wfm_output_cleanup,
    .get_summary_info = wfm_output_get_summary_info,
    .validate_options = wfm_output_validate_options,
    .get_cli_options = wfm_output_get_cli_options,
    .write_chunk = NULL
};

OutputModuleInterface* output_wfm_get_module_api(void) {
    return &s_wfm_output_api;
}
