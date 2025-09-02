/*  iq_correct.c - I/Q imbalance correction and estimation utilities.
 *
 *  This file is part of iq_resample_tool.
 *
 *  Copyright (C) 2025 iq_resample_tool
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
/*
 *
 *  The I/Q correction algorithm in this file is a C port of the
 *  randomized hill-climbing algorithm from the SDR# project.
 *
 *  The original C# code is licensed under the MIT license, and its
 *  copyright and permission notice is included below as required.
 *
 *  Copyright (c) 2012 Youssef Touil and other contributors,
 *  http://sdrsharp.com/
 *
 *  Permission is hereby granted, free of charge, to any person obtaining
 *  a copy of this software and associated documentation files (the
 *  "Software"), to deal in the Software without restriction, including
 *  without limitation the rights to use, copy, modify, merge, publish,
 *  distribute, sublicense, and/or sell copies of the Software, and to
 *  permit persons to whom the Software is furnished to do so, subject to
 *  the following conditions:
 *
 *  The above copyright notice and this permission notice shall be
 *  included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "iq_correct.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "memory_arena.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Forward Declarations for Static Helper Functions ---
static void _apply_correction_to_buffer(complex_float_t* buffer, int length, float gain_adj, float phase_adj);
static float _calculate_imbalance_metric(IqCorrectionResources* iq_res, const complex_float_t* signal_block, float gain_adj, float phase_adj);
static void _estimate_power(IqCorrectionResources* iq_res, const complex_float_t* signal_block);
static float _get_random_direction(void);
static void _calculate_power_spectrum(IqCorrectionResources* iq_res, const complex_float_t* signal_block, float gain_adj, float phase_adj);


// --- Public API Functions ---

bool iq_correct_init(AppConfig* config, AppResources* resources, MemoryArena* arena) {
    if (!config->iq_correction.enable) {
        resources->iq_correction.fft_plan = NULL;
        return true;
    }

    srand((unsigned int)time(NULL));

    if (pthread_mutex_init(&resources->iq_correction.iq_factors_mutex, NULL) != 0) {
        log_fatal("Failed to initialize I/Q correction mutex.");
        return false;
    }

    const unsigned int nfft = IQ_CORRECTION_FFT_SIZE;
    resources->iq_correction.fft_buffer = (complex_float_t*)mem_arena_alloc(arena, nfft * sizeof(complex_float_t), false);
    resources->iq_correction.fft_shift_buffer = (complex_float_t*)mem_arena_alloc(arena, nfft * sizeof(complex_float_t), false);
    resources->iq_correction.spectrum_buffer = (float*)mem_arena_alloc(arena, nfft * sizeof(float), false);
    resources->iq_correction.window_coeffs = (float*)mem_arena_alloc(arena, nfft * sizeof(float), false);
    resources->iq_correction.optimization_accum_buffer = (complex_float_t*)mem_arena_alloc(arena, IQ_CORRECTION_FFT_SIZE * sizeof(complex_float_t), false);

    if (!resources->iq_correction.fft_buffer ||
        !resources->iq_correction.fft_shift_buffer ||
        !resources->iq_correction.spectrum_buffer ||
        !resources->iq_correction.window_coeffs ||
        !resources->iq_correction.optimization_accum_buffer) {
        // mem_arena_alloc logs the fatal error, so we just need to return
        return false;
    }

    // The FFT plan itself is managed by liquid-dsp, not our arena
    resources->iq_correction.fft_plan = fft_create_plan(nfft, (liquid_float_complex*)resources->iq_correction.fft_buffer, (liquid_float_complex*)resources->iq_correction.fft_buffer, LIQUID_FFT_FORWARD, 0);
    if (!resources->iq_correction.fft_plan) {
        log_fatal("Failed to create liquid-dsp FFT plan for I/Q correction.");
        return false;
    }

    for (unsigned int i = 0; i < nfft; i++) {
        resources->iq_correction.window_coeffs[i] = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)(nfft - 1));
    }

    resources->iq_correction.factors_buffer[0].mag = 0.0f;
    resources->iq_correction.factors_buffer[0].phase = 0.0f;
    resources->iq_correction.factors_buffer[1].mag = 0.0f;
    resources->iq_correction.factors_buffer[1].phase = 0.0f;
    atomic_store(&resources->iq_correction.active_buffer_idx, 0);

    resources->iq_correction.average_power = 0.0f;
    resources->iq_correction.power_range = 0.0f;
    resources->iq_correction.samples_in_accum = 0;
    resources->iq_correction.last_optimization_time = 0.0;

    log_info("I/Q Correction enabled");
    return true;
}

void iq_correct_apply(AppResources* resources, complex_float_t* samples, int num_samples) {
    if (!resources->config->iq_correction.enable) return;

    IqCorrectionFactors local_factors;

    pthread_mutex_lock(&resources->iq_correction.iq_factors_mutex);
    int active_idx = atomic_load(&resources->iq_correction.active_buffer_idx);
    local_factors = resources->iq_correction.factors_buffer[active_idx];
    pthread_mutex_unlock(&resources->iq_correction.iq_factors_mutex);

    _apply_correction_to_buffer(samples, num_samples, local_factors.mag, local_factors.phase);
}

void iq_correct_run_optimization(AppResources* resources, const complex_float_t* optimization_data) {
    if (!resources->config->iq_correction.enable) return;

    double current_time = get_monotonic_time_sec();
    double time_since_last_run = (current_time - resources->iq_correction.last_optimization_time) * 1000.0;

    if (time_since_last_run < IQ_CORRECTION_INTERVAL_MS) {
        return;
    }

    log_debug("IQ_OPT_PROBE: Optimization function was called.");

    _estimate_power(&resources->iq_correction, optimization_data);

    const float power_threshold_db = IQ_CORRECTION_POWER_THRESHOLD_DB;
    if (resources->iq_correction.power_range < power_threshold_db) {
        log_debug("IQ_OPT_PROBE: Skipping optimization pass, signal peak-to-average power ratio is too low.");
        return;
    }

    resources->iq_correction.last_optimization_time = current_time;

    log_debug("IQ_OPT_PROBE: Signal is strong enough, starting optimization...");

    int active_idx = atomic_load(&resources->iq_correction.active_buffer_idx);
    float current_gain = resources->iq_correction.factors_buffer[active_idx].mag;
    float current_phase = resources->iq_correction.factors_buffer[active_idx].phase;

    float best_metric = _calculate_imbalance_metric(&resources->iq_correction, optimization_data, current_gain, current_phase);
    log_debug("IQ_OPT_PROBE: Initial metric (utility score) is %.4e", best_metric);

    for (int i = 0; i < IQ_MAX_PASSES; i++) {
        float candidate_gain = current_gain + IQ_BASE_INCREMENT * _get_random_direction();
        float candidate_phase = current_phase + IQ_BASE_INCREMENT * _get_random_direction();
        float candidate_metric = _calculate_imbalance_metric(&resources->iq_correction, optimization_data, candidate_gain, candidate_phase);

        // CRITICAL FIX: We want to MAXIMIZE the metric, not minimize it.
        if (candidate_metric > best_metric) {
            best_metric = candidate_metric;
            current_gain = candidate_gain;
            current_phase = candidate_phase;
        }
    }

    log_debug("IQ_OPT_PROBE: Optimization finished. Best metric found: %.4e", best_metric);
    log_debug("IQ_OPT_PROBE: Final raw params for this pass: mag=%.6f, phase=%.6f", current_gain, current_phase);

    int current_active_idx = atomic_load(&resources->iq_correction.active_buffer_idx);
    int inactive_idx = 1 - current_active_idx;

    float smoothed_gain = ((1.0f - IQ_CORRECTION_SMOOTHING_FACTOR) * resources->iq_correction.factors_buffer[current_active_idx].mag) + (IQ_CORRECTION_SMOOTHING_FACTOR * current_gain);
    float smoothed_phase = ((1.0f - IQ_CORRECTION_SMOOTHING_FACTOR) * resources->iq_correction.factors_buffer[current_active_idx].phase) + (IQ_CORRECTION_SMOOTHING_FACTOR * current_phase);
    
    pthread_mutex_lock(&resources->iq_correction.iq_factors_mutex);
    resources->iq_correction.factors_buffer[inactive_idx].mag = smoothed_gain;
    resources->iq_correction.factors_buffer[inactive_idx].phase = smoothed_phase;
    atomic_store(&resources->iq_correction.active_buffer_idx, inactive_idx);
    pthread_mutex_unlock(&resources->iq_correction.iq_factors_mutex);

    log_debug("IQ_OPT_PROBE: Smoothed global params updated to: mag=%.6f, phase=%.6f", smoothed_gain, smoothed_phase);
}

void iq_correct_destroy(AppResources* resources) {
    if (resources->config->iq_correction.enable) {
        pthread_mutex_destroy(&resources->iq_correction.iq_factors_mutex);
    }
    if (resources->iq_correction.fft_plan) {
        fft_destroy_plan((fftplan)resources->iq_correction.fft_plan);
        resources->iq_correction.fft_plan = NULL;
    }
    // No need to free buffers, they are part of the setup_arena
    resources->iq_correction.fft_buffer = NULL;
    resources->iq_correction.fft_shift_buffer = NULL;
    resources->iq_correction.spectrum_buffer = NULL;
    resources->iq_correction.window_coeffs = NULL;
    resources->iq_correction.optimization_accum_buffer = NULL;
}


// --- Internal Helper Functions ---

static void _apply_correction_to_buffer(complex_float_t* buffer, int length, float gain_adj, float phase_adj) {
    const float magp1 = 1.0f + gain_adj;
    for (int i = 0; i < length; i++) {
        complex_float_t v = buffer[i];
        buffer[i] = (crealf(v) * magp1) + (cimagf(v) + phase_adj * crealf(v)) * I;
    }
}

static void _calculate_power_spectrum(IqCorrectionResources* iq_res, const complex_float_t* signal_block, float gain_adj, float phase_adj) {
    const int nfft = IQ_CORRECTION_FFT_SIZE;
    const int half_nfft = nfft / 2;

    memcpy(iq_res->fft_buffer, signal_block, nfft * sizeof(complex_float_t));
    _apply_correction_to_buffer(iq_res->fft_buffer, nfft, gain_adj, phase_adj);

    for (int i = 0; i < nfft; i++) {
        iq_res->fft_buffer[i] *= iq_res->window_coeffs[i];
    }

    fft_execute((fftplan)iq_res->fft_plan);

    memcpy(iq_res->fft_shift_buffer, iq_res->fft_buffer + half_nfft, half_nfft * sizeof(complex_float_t));
    memcpy(iq_res->fft_shift_buffer + half_nfft, iq_res->fft_buffer, half_nfft * sizeof(complex_float_t));

    for (int i = 0; i < nfft; i++) {
        float mag = cabsf(iq_res->fft_shift_buffer[i]);
        mag /= (float)nfft;
        iq_res->spectrum_buffer[i] = 20.0f * log10f(mag + 1e-12f);
    }
}

static float _calculate_imbalance_metric(IqCorrectionResources* iq_res, const complex_float_t* signal_block, float gain_adj, float phase_adj) {
    const int nfft = IQ_CORRECTION_FFT_SIZE;
    const int half_nfft = nfft / 2;

    _calculate_power_spectrum(iq_res, signal_block, gain_adj, phase_adj);

    float total_utility = 0.0f;
    const int lower_bound = (int)(0.05f * half_nfft);
    const int upper_bound = (int)(0.95f * half_nfft);

    for (int i = lower_bound; i < upper_bound; i++) {
        float p_neg = iq_res->spectrum_buffer[i];
        float p_pos = iq_res->spectrum_buffer[nfft - 1 - i];
        // This check is slightly different from C# but achieves the same goal:
        // only consider bins where there is significant energy above the noise floor.
        if (p_pos > -80.0f || p_neg > -80.0f) {
            float difference = p_pos - p_neg;
            total_utility += difference * difference;
        }
    }
    return total_utility;
}

static void _estimate_power(IqCorrectionResources* iq_res, const complex_float_t* signal_block) {
    const int nfft = IQ_CORRECTION_FFT_SIZE;
    const int half_nfft = nfft / 2;

    _calculate_power_spectrum(iq_res, signal_block, 0.0f, 0.0f);

    float max_power = -1000.0f;
    double avg_power_sum = 0.0;
    int count = 0;
    const int lower_bound = (int)(0.05f * half_nfft);
    const int upper_bound = (int)(0.95f * half_nfft);

    for (int i = lower_bound; i < upper_bound; i++) {
        float p_neg = iq_res->spectrum_buffer[i];
        float p_pos = iq_res->spectrum_buffer[nfft - 1 - i];
        if (p_pos > max_power) max_power = p_pos;
        if (p_neg > max_power) max_power = p_neg;
        avg_power_sum += p_pos + p_neg;
        count += 2;
    }

    if (count > 0) {
        iq_res->average_power = (float)(avg_power_sum / count);
        iq_res->power_range = max_power - iq_res->average_power;
    } else {
        iq_res->average_power = 0.0f;
        iq_res->power_range = 0.0f;
    }
}

static float _get_random_direction(void) {
    return (rand() > (RAND_MAX / 2)) ? 1.0f : -1.0f;
}
