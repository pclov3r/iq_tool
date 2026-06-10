/**
 * @file output_wfm.c
 */

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
#include "constants.h"
#include "audio_output_functions.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "utilities.h"
#include "signal_handler.h"
#include "queue.h"
#include "sample_convert.h"
#include <libredsea.h>
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

// --- Constants ---
#define WFM_BUFFER_SIZE         8192
#define WFM_PILOT_HZ            19000.0f
#define WFM_PLL_BW_HZ           10.0f
#define WFM_PILOT_FIR_USEC      740.0f
#define WFM_PILOT_FIR_HALFBAND  800.0f
#define WFM_AUDIO_FIR_CUTOFF    16500.0f
#define WFM_AUDIO_FIR_LEN_USEC  740.0f
#define WFM_DEEMPH_ORDER        1
#define WFM_STEREO_SEPARATION   1.2f

// --- RDS Configuration Enum ---
typedef enum {
    RDS_STANDARD_RBDS, // US Standard (Default)
    RDS_STANDARD_RDS   // World Standard
} RdsStandard;


// --- Logging Config ---

// --- Gain Staging ---
// Scaling factor applied to the MPX signal after demodulation.
// Standard FM deviation (75kHz) maps to 1.0 in freqdem.
// We scale this down (~ -16.5 dB) to create headroom for the Pilot tone,
// Stereo Matrix (Sum + Diff), and De-emphasis filters.
#define WFM_MPX_SCALING_FACTOR  0.15f

// --- Audio Config ---
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_CHANNELS          2

// --- MPX Rate Constraints ---
#define WFM_MIN_MPX_RATE        192000.0
#define WFM_MAX_MPX_RATE        384000.0
#define WFM_DEFAULT_MPX_RATE    240000.0

// --- Internal Structures ---

typedef struct {
    float buffer[WFM_BUFFER_SIZE];
    float sum;
    int index;
} RunningAverage;

typedef struct {
    float coeff_B[3 * ((WFM_DEEMPH_ORDER + 1) / 2)];
    float coeff_A[3 * ((WFM_DEEMPH_ORDER + 1) / 2)];
    iirfilt_rrrf iir_l;
    iirfilt_rrrf iir_r;
} DeEmphasis;

typedef struct {
    // Pipeline State
    AudioOutputContext* audio_out;

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
    LibRedseaHandle redsea;
    RdsState last_rds_state;
    size_t rds_display_counter;
    size_t rds_display_threshold;
} WfmContext;

// --- CLI Config ---
static struct {
    float deemph_us;
    float gain_val;
    bool force_stereo;
    bool force_mono;
    bool raw_mpx_stdout;
    bool rds_disable;
    RdsStandard rds_standard; // The final, resolved standard after parsing CLI flags
    bool use_world_rds;        // Temporary flag from CLI to select the non-default standard


} s_wfm_config = {
    .deemph_us = 75.0f,
    .gain_val = 5.0f,
    .force_stereo = 0,
    .force_mono = 0,
    .raw_mpx_stdout = 0,
    .rds_disable = 0,
    .rds_standard = RDS_STANDARD_RBDS, // Default to US standard
    .use_world_rds = 0,


};

// --- Helpers ---

static float angular_freq(float hertz, float sample_rate) {
    return hertz * 2.0f * (float)M_PI / sample_rate;
}

static void running_average_init(RunningAverage* ra) {
    memset(ra->buffer, 0, sizeof(ra->buffer));
    ra->sum = 0.0f;
    ra->index = 0;
    // Pre-fill as done in demux.cpp
    for (int i = 0; i < WFM_BUFFER_SIZE; i++) {
        ra->sum -= ra->buffer[ra->index];
        ra->buffer[ra->index] = 9.0f;
        ra->sum += ra->buffer[ra->index];
        ra->index = (ra->index + 1) % WFM_BUFFER_SIZE;
    }
}

static void running_average_push(RunningAverage* ra, float val) {
    ra->sum -= ra->buffer[ra->index];
    ra->buffer[ra->index] = val;
    ra->sum += ra->buffer[ra->index];
    ra->index = (ra->index + 1) % WFM_BUFFER_SIZE;
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

// --- Module Interface Implementation ---

static bool wfm_output_validate_options(AppConfig* config) {
    // Resolve the user's choice from the CLI flag into our clean enum state.
    if (s_wfm_config.use_world_rds) {
        s_wfm_config.rds_standard = RDS_STANDARD_RDS;
    }

    // 1. Force Pipeline Format to CF32
    // We need high-precision float I/Q for the FM demodulator.
    config->baseband_sample_format.format = CF32;

    // 2. Validate/Enforce MPX Rate
    if (config->baseband_sample_rate.rate_hz == 0.0) {
        // If the user didn't request a specific rate, ask the pipeline to give us our default.
        config->baseband_sample_rate.rate_hz = WFM_DEFAULT_MPX_RATE;
        config->baseband_sample_rate.provided = true; // Mark as provided so setup.c respects it
    } else {
        // Case B: User specified a rate. Validate range.
        double rate = config->baseband_sample_rate.rate_hz;
        if (rate < WFM_MIN_MPX_RATE || rate > WFM_MAX_MPX_RATE) {
            log_error("WFM: Invalid MPX rate %.15g Hz.", rate);
            log_error("Valid range is %.15g Hz to %.15g Hz.", WFM_MIN_MPX_RATE, WFM_MAX_MPX_RATE);
            return false;
        }
    }

    // 3. Check for conflicting forced modes
    if (s_wfm_config.force_stereo && s_wfm_config.force_mono) {
        log_error("WFM: Cannot force both Stereo and Mono simultaneously. Please choose one.");
        return false;
    }

    return true;
}

static bool wfm_output_initialize(ModuleContext* context) {
    AppContext* res = context->app;

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

    WfmContext* wfm_decoder = (WfmContext*)mem_arena_alloc(&res->pipeline.setup_arena, sizeof(WfmContext), true);
    if (!wfm_decoder) return false;
    res->module.output_private_data = wfm_decoder;

    wfm_decoder->audio_out = audio_output_create(&res->pipeline.setup_arena, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, res->module.source_info.demod_audio_buffer_size, context->config->audio.writer_path, context->config->audio.writer_rf64, context->config->audio.mute);
    if (!wfm_decoder->audio_out) return false;

    // 3. DSP Configuration
    float mpx_rate = (float)context->config->baseband_sample_rate.rate_hz;
    wfm_decoder->input_samplerate = mpx_rate;
    wfm_decoder->gain = s_wfm_config.gain_val;
    log_info("WFM: Baseband %.15g Hz | Audio %d Hz | De-emphasis %.15g us",
             mpx_rate, AUDIO_SAMPLE_RATE, s_wfm_config.deemph_us);

    // 4. Create Liquid Objects

    float deviation = 75000.0f;
    // Engineering Note: We use the correct modulation index math here (deviation / rate).
    // This results in a "hot" signal (75kHz = 1.0), so we apply WFM_MPX_SCALING_FACTOR
    // in the writer loop to create headroom for stereo processing.
    float kf = deviation / mpx_rate;
    wfm_decoder->fm_demod = freqdem_create(kf);

    wfm_decoder->nco_pilot_approx = nco_crcf_create(LIQUID_VCO);
    nco_crcf_set_frequency(wfm_decoder->nco_pilot_approx, angular_freq(WFM_PILOT_HZ, mpx_rate));

    wfm_decoder->nco_pilot_exact = nco_crcf_create(LIQUID_VCO);
    nco_crcf_set_frequency(wfm_decoder->nco_pilot_exact, angular_freq(WFM_PILOT_HZ, mpx_rate));
    nco_crcf_pll_set_bandwidth(wfm_decoder->nco_pilot_exact, WFM_PLL_BW_HZ / mpx_rate);

    wfm_decoder->nco_stereo_subcarrier = nco_crcf_create(LIQUID_VCO);
    nco_crcf_set_frequency(wfm_decoder->nco_stereo_subcarrier, 2.0f * angular_freq(WFM_PILOT_HZ, mpx_rate));

    // Filters
    int pilot_fir_half_length = (int)(mpx_rate * 1e-6f * WFM_PILOT_FIR_USEC);
    int pilot_fir_length = pilot_fir_half_length * 2 + 1;
    wfm_decoder->fir_pilot = firfilt_crcf_create_kaiser(pilot_fir_length, WFM_PILOT_FIR_HALFBAND / mpx_rate, context->config->dsp.filter.args.attenuation, 0.0f);
    firfilt_crcf_set_scale(wfm_decoder->fir_pilot, 2.0f * (WFM_PILOT_FIR_HALFBAND / mpx_rate));

    int audio_fir_length = (int)(WFM_AUDIO_FIR_LEN_USEC * 1e-6f * mpx_rate);
    if (audio_fir_length % 2 == 0) audio_fir_length++;
    float audio_fc = WFM_AUDIO_FIR_CUTOFF / mpx_rate;

    wfm_decoder->fir_sum = firfilt_rrrf_create_kaiser(audio_fir_length, audio_fc, context->config->dsp.filter.args.attenuation, 0.0f);
    firfilt_rrrf_set_scale(wfm_decoder->fir_sum, 2.0f * audio_fc);

    wfm_decoder->fir_diff = firfilt_rrrf_create_kaiser(audio_fir_length, audio_fc, context->config->dsp.filter.args.attenuation, 0.0f);
    firfilt_rrrf_set_scale(wfm_decoder->fir_diff, 2.0f * audio_fc);

    deemphasis_init(&wfm_decoder->deemphasis, s_wfm_config.deemph_us, mpx_rate);
    running_average_init(&wfm_decoder->pilotnoise);

    // Output Resamplers
    wfm_decoder->output_resample_ratio = (float)AUDIO_SAMPLE_RATE / mpx_rate;
    wfm_decoder->resamp_out_l = msresamp_rrrf_create(wfm_decoder->output_resample_ratio, context->config->dsp.filter.args.attenuation);
    wfm_decoder->resamp_out_r = msresamp_rrrf_create(wfm_decoder->output_resample_ratio, context->config->dsp.filter.args.attenuation);

    // 5. Scratch Buffers (Local Elastic Allocation)
    // We calculate the maximum buffer size required for ANY stage of this specific module.
    // If upsampling (e.g. 32k -> 48k), the output buffer needs more space than the input.
    size_t buf_samples = res->pipeline.alloc_size_samples;
    size_t out_buf_samples = (size_t)ceil(buf_samples * wfm_decoder->output_resample_ratio) + 128;
    size_t max_dsp_samples = (buf_samples > out_buf_samples) ? buf_samples : out_buf_samples;

    wfm_decoder->mpx_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    wfm_decoder->audio_out_l = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);
    wfm_decoder->audio_out_r = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(float), false);

    // Interleaved buffer is always sized for the output
    wfm_decoder->interleaved_pcm = mem_arena_alloc(&res->pipeline.setup_arena, out_buf_samples * 2 * sizeof(int16_t), false);

    if (!wfm_decoder->mpx_buffer || !wfm_decoder->audio_out_l || !wfm_decoder->audio_out_r || !wfm_decoder->interleaved_pcm) return false;

    // Optional: Allocate S16 buffer for MPX stdout if requested
    if (s_wfm_config.raw_mpx_stdout) {
        wfm_decoder->mpx_s16_buffer = mem_arena_alloc(&res->pipeline.setup_arena, max_dsp_samples * sizeof(int16_t), false);
        if (!wfm_decoder->mpx_s16_buffer) return false;
    }

    // 6. Initialize RDS (Enabled unless disabled)
    if (!s_wfm_config.rds_disable) {
        // The `is_rbds` parameter for redsea is determined by our final enum state.
        bool is_rbds = (s_wfm_config.rds_standard == RDS_STANDARD_RBDS);
        wfm_decoder->redsea = libredsea_init(wfm_decoder->input_samplerate, is_rbds, &res->pipeline.setup_arena);
        memset(&wfm_decoder->last_rds_state, 0, sizeof(RdsState));

        wfm_decoder->rds_display_counter = 0;
        wfm_decoder->rds_display_threshold = (size_t)(wfm_decoder->input_samplerate * CONSOLE_UPDATE_INTERVAL_SEC); // 1 second

        log_info("WFM: RDS Decoder enabled (Standard: %s)",
                 is_rbds ? "RBDS (US, Default)" : "RDS (World)");
    } else {
        wfm_decoder->redsea = NULL;
    }


    return true;
}

static void wfm_output_reset(ModuleContext* context) { (void)context; /* TODO: Reset PLL state */ }

static void wfm_output_flush(ModuleContext* context) {
    WfmContext* wfm_decoder = (WfmContext*)context->app->module.output_private_data;
    audio_output_clear(wfm_decoder->audio_out);
}
static size_t wfm_output_write_chunk(ModuleContext* context, const void* buffer, size_t input_bytes) {
    AppContext* res = context->app;
    WfmContext* wfm_decoder = (WfmContext*)res->module.output_private_data;

    static size_t stat_counter = 0;
    static double accum_mag_sum = 0.0, accum_mag_sq_sum = 0.0, accum_pilot_mag_sum = 0.0;
    static double accum_stereo_pct_sum = 0.0, accum_pilot_err_sq_sum = 0.0;
    static size_t accum_pilot_count = 0;

    static size_t stat_rate_threshold = 0;
    static bool _first_run = true;
    if (_first_run) {
        stat_rate_threshold = (size_t)(wfm_decoder->input_samplerate * CONSOLE_UPDATE_INTERVAL_SEC);
        _first_run = false;
    }

    if (input_bytes == 0) return 0;

    unsigned int num_frames = input_bytes / res->module.output_bytes_per_iq_sample;
    ComplexFloat* iq_in = (ComplexFloat*)buffer;
    liquid_float_complex* iq_ptr = (liquid_float_complex*)iq_in;

    for (unsigned int k = 0; k < num_frames; k++) {
        float mag = cabsf(iq_ptr[k]);
        accum_mag_sum += mag;
        accum_mag_sq_sum += (mag * mag);
    }
    stat_counter += num_frames;

    freqdem_demodulate_block(wfm_decoder->fm_demod, iq_ptr, num_frames, wfm_decoder->mpx_buffer);

    if (s_wfm_config.raw_mpx_stdout && wfm_decoder->mpx_s16_buffer) {
        for (unsigned int k = 0; k < num_frames; k++) {
            float sample = wfm_decoder->mpx_buffer[k] * 32767.0f;
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            wfm_decoder->mpx_s16_buffer[k] = (int16_t)sample;
        }
        fwrite(wfm_decoder->mpx_s16_buffer, sizeof(int16_t), num_frames, stdout);
    }

    if (wfm_decoder->redsea) {
        RdsState current;
        libredsea_process_mpx(wfm_decoder->redsea, wfm_decoder->mpx_buffer, num_frames, &current);
        wfm_decoder->rds_display_counter += num_frames;
        if (wfm_decoder->rds_display_counter >= wfm_decoder->rds_display_threshold && current.valid) {
            wfm_decoder->rds_display_counter = 0;
            char clean_rt[65];
            for (int i = 0; i < 64; i++) {
                char c = current.radiotext[i];
                if (c == 0x0D) {
                    clean_rt[i] = '\0';
                    break;
                }
                clean_rt[i] = c ? c : ' ';
            }
            clean_rt[64] = '\0';

            char clean_ps[9];
            for (int i = 0; i < 8; i++) {
                char c = current.ps_name[i];
                if (c == 0x0D) {
                    clean_ps[i] = '\0';
                    break;
                }
                clean_ps[i] = c ? c : ' ';
            }
            clean_ps[8] = '\0';

            char clean_ptyn[9];
            for (int i = 0; i < 8; i++) {
                char c = current.pty_name[i];
                if (c == 0x0D) {
                    clean_ptyn[i] = '\0';
                    break;
                }
                clean_ptyn[i] = c ? c : ' ';
            }
            clean_ptyn[8] = '\0';

            // Trim trailing spaces
            size_t rt_length = strlen(clean_rt);
            while (rt_length > 0 && clean_rt[rt_length - 1] == ' ') { clean_rt[rt_length - 1] = '\0'; rt_length--; }
            size_t ps_length = strlen(clean_ps);
            while (ps_length > 0 && clean_ps[ps_length - 1] == ' ') { clean_ps[ps_length - 1] = '\0'; ps_length--; }
            size_t ptyn_length = strlen(clean_ptyn);
            while (ptyn_length > 0 && clean_ptyn[ptyn_length - 1] == ' ') { clean_ptyn[ptyn_length - 1] = '\0'; ptyn_length--; }

            // Trim leading spaces
            char* rt_ptr = clean_rt;
            while (*rt_ptr == ' ') rt_ptr++;
            char* ps_ptr = clean_ps;
            while (*ps_ptr == ' ') ps_ptr++;

            char main_af_buf[128] = "";
            if (current.alt_freq_count > 0) {
                int offset = snprintf(main_af_buf, sizeof(main_af_buf), "AF: ");
                for (int f = 0; f < current.alt_freq_count && (size_t)offset < sizeof(main_af_buf) - 10; f++) {
                    offset += snprintf(main_af_buf + offset, sizeof(main_af_buf) - offset, "%.1f%s",
                                       current.alt_freqs[f] / 1000.0,
                                       (f < current.alt_freq_count - 1) ? ", " : "");
                }
            }

            char iso_buf[32] = "";
            if (current.country_code[0] != '\0' && current.country_code[0] != '-') {
                snprintf(iso_buf, sizeof(iso_buf), " | ECC: %s", current.country_code);
            }

            char ptyn_buf[32] = "";
            if (ptyn_length > 0) {
                snprintf(ptyn_buf, sizeof(ptyn_buf), " | PTYN: %s", clean_ptyn);
            }

            if (s_wfm_config.rds_standard == RDS_STANDARD_RBDS && current.callsign[0] != '\0') {
                 log_info("RBDS PI: %04X | CALL: %s%s",
                         current.pi_code, current.callsign, iso_buf);
            } else {
                 log_info("RDS PI: %04X%s",
                         current.pi_code, iso_buf);
            }

            if (main_af_buf[0] != '\0') {
                log_info("%s %s", current.is_rbds ? "RBDS" : "RDS", main_af_buf);
            }

            int has_tmc = 0;
            for (int i = 0; i < 32; i++) {
                if (current.oda_app_for_group[i] == 0xCD46 || current.oda_app_for_group[i] == 0xCD47) {
                    has_tmc = 1;
                    break;
                }
            }

            log_info("%s FLAGS: TP: %d | TA: %d | MS: %d | ST: %d | CMP: %d | DYN: %d | TMC: %d",
                    current.is_rbds ? "RBDS" : "RDS", current.tp, current.ta, current.is_music, current.stereo, current.compressed, current.dynamic, has_tmc);

            log_info("%s PTY: %s%s", current.is_rbds ? "RBDS" : "RDS", current.program_type, ptyn_buf);

            if (ps_ptr[0] != '\0') {
                 log_info("%s PS: %s", current.is_rbds ? "RBDS" : "RDS", ps_ptr);
            }

            if (rt_ptr[0] != '\0') {
                 log_info("%s RT: %s", current.is_rbds ? "RBDS" : "RDS", rt_ptr);
            }

            for (int e = 0; e < current.rt_plus_event_count; e++) {
                RdsRTPlusEvent* event = &current.rt_plus_events[e];

                size_t t_length = strlen(event->title);
                while (t_length > 0 && (event->title[t_length - 1] == ' ' || event->title[t_length - 1] == '\r')) { event->title[t_length - 1] = '\0'; t_length--; }
                size_t a_length = strlen(event->artist);
                while (a_length > 0 && (event->artist[a_length - 1] == ' ' || event->artist[a_length - 1] == '\r')) { event->artist[a_length - 1] = '\0'; a_length--; }

                bool is_dup = false;
                for (int i = 0; i < e; i++) {
                    if (strcmp(current.rt_plus_events[i].artist, event->artist) == 0 &&
                        strcmp(current.rt_plus_events[i].title, event->title) == 0) {
                        is_dup = true; break;
                    }
                }
                for (int i = 0; i < wfm_decoder->last_rds_state.rt_plus_event_count; i++) {
                    if (strcmp(wfm_decoder->last_rds_state.rt_plus_events[i].artist, event->artist) == 0 &&
                        strcmp(wfm_decoder->last_rds_state.rt_plus_events[i].title, event->title) == 0) {
                        is_dup = true; break;
                    }
                }
                if (is_dup) continue;

                if (event->title[0] != '\0' && event->artist[0] != '\0') {
                    log_info("%s RT+: Artist=%s | Title=%s", current.is_rbds ? "RBDS" : "RDS", event->artist, event->title);
                } else if (event->artist[0] != '\0') {
                    log_info("%s RT+: Artist=%s", current.is_rbds ? "RBDS" : "RDS", event->artist);
                } else if (event->title[0] != '\0') {
                    log_info("%s RT+: Title=%s", current.is_rbds ? "RBDS" : "RDS", event->title);
                }
            }

            if (current.clock_time[0] != '\0' && strcmp(current.clock_time, wfm_decoder->last_rds_state.clock_time) != 0) {
                log_info("%s CT: %s", current.is_rbds ? "RBDS" : "RDS", current.clock_time);
            }

            for (int e = 0; e < current.tmc_event_count; e++) {
                RdsTmcEvent* event = &current.tmc_events[e];

                bool is_dup = false;
                for (int i = 0; i < e; i++) {
                    if (current.tmc_events[i].location_id == event->location_id &&
                        current.tmc_events[i].event_code == event->event_code &&
                        current.tmc_events[i].supplementary_code == event->supplementary_code) {
                        is_dup = true; break;
                    }
                }
                for (int i = 0; i < wfm_decoder->last_rds_state.tmc_event_count; i++) {
                    if (wfm_decoder->last_rds_state.tmc_events[i].location_id == event->location_id &&
                        wfm_decoder->last_rds_state.tmc_events[i].event_code == event->event_code &&
                        wfm_decoder->last_rds_state.tmc_events[i].supplementary_code == event->supplementary_code) {
                        is_dup = true; break;
                    }
                }
                if (is_dup) continue;

                if (event->supplementary_code > 0) {
                    log_info("%s TMC: Location: %u | Event: %u %s | Supplemental: %u %s | Extent: %d | Dir: %d | Div: %d | Dur: %d",
                        current.is_rbds ? "RBDS" : "RDS",
                        event->location_id, event->event_code, event->event_description,
                        event->supplementary_code, get_tmc_supplementary_description(event->supplementary_code),
                        event->extent, event->direction, event->diversion_advised, event->duration);
                } else {
                    log_info("%s TMC: Location: %u | Event: %u %s | Extent: %d | Dir: %d | Div: %d | Dur: %d",
                        current.is_rbds ? "RBDS" : "RDS",
                        event->location_id, event->event_code, event->event_description,
                        event->extent, event->direction, event->diversion_advised, event->duration);
                }
            }

            for (int e = 0; e < current.tdc_event_count; e++) {
                RdsTdcEvent* event = &current.tdc_events[e];
                if (event->data_length == 4) {
                    log_info("%s TDC (5A): Channel=%u | Hex=%02X %02X %02X %02X",
                             current.is_rbds ? "RBDS" : "RDS", event->channel,
                             event->data[0], event->data[1], event->data[2], event->data[3]);
                } else {
                    log_info("%s TDC (5B): Channel=%u | Hex=%02X %02X",
                             current.is_rbds ? "RBDS" : "RDS", event->channel,
                             event->data[0], event->data[1]);
                }
            }

            for (int e = 0; e < current.iha_event_count; e++) {
                RdsIhaEvent* event = &current.iha_events[e];
                if (event->data_length == 4) {
                    log_info("%s IHA (6A): Addr=%u | Hex=%02X %02X %02X %02X",
                             current.is_rbds ? "RBDS" : "RDS", event->address,
                             event->data[0], event->data[1], event->data[2], event->data[3]);
                } else {
                    log_info("%s IHA (6B): Addr=%u | Hex=%02X %02X",
                             current.is_rbds ? "RBDS" : "RDS", event->address,
                             event->data[0], event->data[1]);
                }
            }

            libredsea_clear_events(wfm_decoder->redsea);

            for (int i = 0; i < MAX_EON_NETWORKS; i++) {
                if (current.eon.networks[i].is_valid && current.eon.networks[i].is_update) {
                    bool changed = false;
                    RdsEonNetwork* cur_net = &current.eon.networks[i];
                    RdsEonNetwork* last_net = &wfm_decoder->last_rds_state.eon.networks[i];

                    if (!last_net->is_valid) changed = true;
                    else if (cur_net->pi != last_net->pi) changed = true;
                    else if (strcmp(cur_net->ps, last_net->ps) != 0) changed = true;
                    else if (cur_net->tp != last_net->tp) changed = true;
                    else if (cur_net->ta != last_net->ta) changed = true;
                    else if (cur_net->pty != last_net->pty) changed = true;
                    else if (cur_net->mapped_freq_khz != last_net->mapped_freq_khz) changed = true;
                    else if (cur_net->alt_freq_count != last_net->alt_freq_count) changed = true;
                    else {
                        for (int f = 0; f < cur_net->alt_freq_count; f++) {
                            if (cur_net->alt_freqs[f] != last_net->alt_freqs[f]) {
                                changed = true;
                                break;
                            }
                        }
                    }

                    if (!changed) continue;
                    char af_buf[128] = "";
                    if (current.eon.networks[i].mapped_freq_khz > 0) {
                        snprintf(af_buf, sizeof(af_buf), " | AF=%.1f", current.eon.networks[i].mapped_freq_khz / 1000.0);
                    } else if (current.eon.networks[i].alt_freq_count > 0) {
                        int offset = snprintf(af_buf, sizeof(af_buf), " | AF=");
                        for (int f = 0; f < current.eon.networks[i].alt_freq_count && (size_t)offset < sizeof(af_buf) - 10; f++) {
                            offset += snprintf(af_buf + offset, sizeof(af_buf) - offset, "%.1f%s",
                                               current.eon.networks[i].alt_freqs[f] / 1000.0,
                                               (f < current.eon.networks[i].alt_freq_count - 1) ? ", " : "");
                        }
                    }

                    char eon_ps_buf[9];
                    strncpy(eon_ps_buf, current.eon.networks[i].ps, 8);
                    eon_ps_buf[8] = '\0';
                    size_t eon_ps_length = strlen(eon_ps_buf);
                    while (eon_ps_length > 0 && (eon_ps_buf[eon_ps_length - 1] == ' ' || eon_ps_buf[eon_ps_length - 1] == '\r')) {
                        eon_ps_buf[eon_ps_length - 1] = '\0';
                        eon_ps_length--;
                    }

                    log_info("%s EON: Network PI=0x%04X | PS: %s | TP=%d | TA=%d | PTY=%u%s",
                             current.is_rbds ? "RBDS" : "RDS",
                             current.eon.networks[i].pi, eon_ps_buf,
                             current.eon.networks[i].tp,
                             current.eon.networks[i].ta,
                             current.eon.networks[i].pty,
                             af_buf);

                    current.eon.networks[i].is_update = false;
                }
            }

            wfm_decoder->last_rds_state = current;
        }
    }

    for (unsigned int i = 0; i < num_frames; i++) {
        float insample = wfm_decoder->mpx_buffer[i] * WFM_MPX_SCALING_FACTOR;
        liquid_float_complex pilot_mix_down;
        nco_crcf_mix_down(wfm_decoder->nco_pilot_approx, insample + 0.0f * I, &pilot_mix_down);
        firfilt_crcf_push(wfm_decoder->fir_pilot, pilot_mix_down);
        liquid_float_complex fir_out;
        firfilt_crcf_execute(wfm_decoder->fir_pilot, &fir_out);
        accum_pilot_mag_sum += cabsf(fir_out);
        liquid_float_complex pilot;
        nco_crcf_mix_up(wfm_decoder->nco_pilot_approx, fir_out, &pilot);
        nco_crcf_step(wfm_decoder->nco_pilot_approx);
        float pilot_phase = nco_crcf_get_phase(wfm_decoder->nco_pilot_exact);
        nco_crcf_set_phase(wfm_decoder->nco_stereo_subcarrier, 2.0f * pilot_phase);
        liquid_float_complex pll_val;
        nco_crcf_cexpf(wfm_decoder->nco_pilot_exact, &pll_val);
        float phase_error = cargf(pilot * conjf(pll_val));
        if (i % 4 == 0) {
            nco_crcf_pll_step(wfm_decoder->nco_pilot_exact, phase_error);
            accum_pilot_err_sq_sum += (phase_error * phase_error);
            accum_pilot_count++;
            running_average_push(&wfm_decoder->pilotnoise, phase_error * phase_error);
        }
        nco_crcf_step(wfm_decoder->nco_pilot_exact);
        float stereogain = s_wfm_config.force_mono ? 0.0f : (s_wfm_config.force_stereo ? 1.0f : fmaxf(0.0f, fminf(1.0f, WFM_STEREO_SEPARATION - running_average_get(&wfm_decoder->pilotnoise))));
        accum_stereo_pct_sum += (stereogain * 100.0f);
        firfilt_rrrf_push(wfm_decoder->fir_sum, insample);
        liquid_float_complex sc_mix;
        nco_crcf_mix_down(wfm_decoder->nco_stereo_subcarrier, insample + 0.0f * I, &sc_mix);
        firfilt_rrrf_push(wfm_decoder->fir_diff, cimagf(sc_mix));
        float sum, diff;
        firfilt_rrrf_execute(wfm_decoder->fir_sum, &sum);
        firfilt_rrrf_execute(wfm_decoder->fir_diff, &diff);
        diff = 2.0f * diff * stereogain;
        float left = (sum + diff) * wfm_decoder->gain;
        float right = (sum - diff) * wfm_decoder->gain;
        deemphasis_execute(&wfm_decoder->deemphasis, left, right, &left, &right);
        wfm_decoder->audio_out_l[i] = left;
        wfm_decoder->audio_out_r[i] = right;
    }

        if (stat_counter >= stat_rate_threshold) {
        double avg_power = accum_mag_sq_sum / (double)stat_counter;
        float dbfs = 10.0f * log10f((float)avg_power + 1e-10f);

        double mean_mag = accum_mag_sum / (double)stat_counter;
        float snr_db = 10.0f * log10f((float)((mean_mag*mean_mag) / fmax(1e-10, avg_power - (mean_mag*mean_mag))));
        float avg_pilot_mse = (accum_pilot_count > 0) ? (float)(accum_pilot_err_sq_sum / (double)accum_pilot_count) : 0.0f;
        float pilot_pct = sqrtf(avg_pilot_mse) * 100.0f;
        double avg_pilot = accum_pilot_mag_sum / (double)stat_counter;
        bool is_mono_station = (avg_pilot < 0.001) || (pilot_pct > 100.0f);
        float avg_stereo_pct = (float)(accum_stereo_pct_sum / (double)stat_counter);
        if (avg_stereo_pct > 1.0f) is_mono_station = false;

        if (is_mono_station || s_wfm_config.force_mono) {
            if (wfm_decoder->redsea) {
                const char *rds_std = (s_wfm_config.rds_standard == RDS_STANDARD_RBDS) ? "RBDS" : "RDS";
                if (libredsea_get_sync(wfm_decoder->redsea)) {
                    float bler = libredsea_get_bler(wfm_decoder->redsea);
                    log_info("dBFS: %5.1f | SNR: %.1f dB | Stereo: Mono | %s BER: %.1f%%", dbfs, snr_db, rds_std, bler);
                } else {
                    log_info("dBFS: %5.1f | SNR: %.1f dB | Stereo: Mono | %s BER: no-sync", dbfs, snr_db, rds_std);
                }
            } else {
                log_info("dBFS: %5.1f | SNR: %.1f dB | Stereo: Mono", dbfs, snr_db);
            }
        } else {
            if (wfm_decoder->redsea) {
                const char *rds_std = (s_wfm_config.rds_standard == RDS_STANDARD_RBDS) ? "RBDS" : "RDS";
                if (libredsea_get_sync(wfm_decoder->redsea)) {
                    float bler = libredsea_get_bler(wfm_decoder->redsea);
                    log_info("dBFS: %5.1f | SNR: %.1f dB | Stereo: %.1f%% | Pilot Err: %.1f%% | %s BER: %.1f%%", dbfs, snr_db, avg_stereo_pct, pilot_pct, rds_std, bler);
                } else {
                    log_info("dBFS: %5.1f | SNR: %.1f dB | Stereo: %.1f%% | Pilot Err: %.1f%% | %s BER: no-sync", dbfs, snr_db, avg_stereo_pct, pilot_pct, rds_std);
                }
            } else {
                log_info("dBFS: %5.1f | SNR: %.1f dB | Stereo: %.1f%% | Pilot Err: %.1f%%", dbfs, snr_db, avg_stereo_pct, pilot_pct);
            }
        }
        fprintf(stderr, "\n");
        stat_counter = 0; accum_mag_sum = 0.0; accum_mag_sq_sum = 0.0; accum_pilot_mag_sum = 0.0;
        accum_stereo_pct_sum = 0.0; accum_pilot_err_sq_sum = 0.0; accum_pilot_count = 0;
    }

    unsigned int num_resampled;
    msresamp_rrrf_execute(wfm_decoder->resamp_out_l, wfm_decoder->audio_out_l, num_frames, wfm_decoder->audio_out_l, &num_resampled);
    msresamp_rrrf_execute(wfm_decoder->resamp_out_r, wfm_decoder->audio_out_r, num_frames, wfm_decoder->audio_out_r, &num_resampled);
    sample_convert_interleave_f32_to_s16(wfm_decoder->audio_out_l, wfm_decoder->audio_out_r, wfm_decoder->interleaved_pcm, num_resampled);
    audio_output_write(wfm_decoder->audio_out, wfm_decoder->interleaved_pcm, num_resampled * 2 * sizeof(int16_t), res->pipeline_mode);
    return input_bytes;
}

static void wfm_output_cleanup(ModuleContext* context) {
    AppContext* res = context->app;
    if (!res->module.output_private_data) return;
    WfmContext* wfm_decoder = (WfmContext*)res->module.output_private_data;

    audio_output_destroy(wfm_decoder->audio_out);

    if (wfm_decoder->fm_demod) freqdem_destroy(wfm_decoder->fm_demod);
    if (wfm_decoder->nco_pilot_approx) nco_crcf_destroy(wfm_decoder->nco_pilot_approx);
    if (wfm_decoder->nco_pilot_exact) nco_crcf_destroy(wfm_decoder->nco_pilot_exact);
    if (wfm_decoder->nco_stereo_subcarrier) nco_crcf_destroy(wfm_decoder->nco_stereo_subcarrier);
    if (wfm_decoder->fir_pilot) firfilt_crcf_destroy(wfm_decoder->fir_pilot);
    if (wfm_decoder->fir_sum) firfilt_rrrf_destroy(wfm_decoder->fir_sum);
    if (wfm_decoder->fir_diff) firfilt_rrrf_destroy(wfm_decoder->fir_diff);
    deemphasis_destroy(&wfm_decoder->deemphasis);
    if (wfm_decoder->resamp_out_l) msresamp_rrrf_destroy(wfm_decoder->resamp_out_l);
    if (wfm_decoder->resamp_out_r) msresamp_rrrf_destroy(wfm_decoder->resamp_out_r);

    if (wfm_decoder->redsea) {
        libredsea_free(wfm_decoder->redsea);
        wfm_decoder->redsea = NULL;
    }

}

static void wfm_output_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
    add_summary_item(info, "Output Type", "WFM Stereo Audio");
    add_summary_item(info, "Audio Sample Rate", "%d Hz", AUDIO_SAMPLE_RATE);
    add_summary_item(info, "De-emphasis", "%.0f us", s_wfm_config.deemph_us);

    const char* mode = "Adaptive";
    if (s_wfm_config.force_mono) mode = "Forced Mono";
    if (s_wfm_config.force_stereo) mode = "Forced Stereo";
    add_summary_item(info, "Stereo Mode", "%s", mode);
}

const struct argparse_option wfm_output_cli_options[] = {
    OPT_GROUP("WFM Output (wfm)"),
    OPT_FLOAT(0, "wfm-de-emphasis-time", &s_wfm_config.deemph_us, "Set FM de-emphasis time constant in microseconds (default: 75.0).", NULL, 0, 0),
    OPT_FLOAT(0, "wfm-gain", &s_wfm_config.gain_val, "Set audio output gain (linear).", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-force-stereo", &s_wfm_config.force_stereo, "Force stereo decoding regardless of signal quality.", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-force-mono", &s_wfm_config.force_mono, "Force mono output.", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-raw-mpx-stdout", &s_wfm_config.raw_mpx_stdout, "Pipe raw MPX data (S16 Mono) to stdout while playing audio.", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-no-rds", &s_wfm_config.rds_disable, "Disable RDS decoding (Enabled by default).", NULL, 0, 0),
    OPT_BOOLEAN(0, "wfm-rds", &s_wfm_config.use_world_rds, "Use World RDS standard (Default is US RBDS).", NULL, 0, 0),


};

const struct argparse_option* wfm_output_get_cli_options(int* count) {
    *count = sizeof(wfm_output_cli_options) / sizeof(wfm_output_cli_options[0]);
    return wfm_output_cli_options;
}

static OutputModuleInterface s_wfm_output_api = {
    .initialize = wfm_output_initialize,
    .write_chunk = wfm_output_write_chunk,
    .reset = wfm_output_reset,
    .flush = wfm_output_flush,
    .cleanup = wfm_output_cleanup,
    .get_summary_info = wfm_output_get_summary_info,
    .validate_options = wfm_output_validate_options,
    .get_cli_options = wfm_output_get_cli_options,
};

OutputModuleInterface* output_wfm_get_module_api(void) {
    return &s_wfm_output_api;
}
