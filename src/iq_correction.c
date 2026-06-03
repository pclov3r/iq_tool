/*
 * Original Work Copyright (c) 2016-2023, Youssef Touil <youssef@airspy.com>
 * Original Work Copyright (c) 2018, Leif Asbrink <leif@sm5bsz.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
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

#include "iq_correction.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "mem_arena.h"
#include "utilities.h"
#include "pre_processor.h" // Needed for initial calibration chain
#include "sample_convert.h"
#include <stdatomic.h> // Needed for byte size calculations
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <pthread.h>
#include <liquid.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Constants  ---
// We alias the global constant to FFTBins to keep the math formulas identical to the original source.
#define FFTBins IQ_CORRECTION_FFT_SIZE

#define BoostFactor 100000.0
#define BinsToOptimize (FFTBins/25)
#define EdgeBinsToSkip (FFTBins/22)
#define CenterBinsToSkip 2
#define MaxLookback 4
#define PhaseStep 1e-2f
#define AmplitudeStep 1e-2f
#define MaxMu 50.0f
#define MinDeltaMu 0.1f
#define MinimumPower 0.01f
#define PowerThreshold 0.5f
#define BuffersToSkipOnReset 2
#define MaxPowerDecay 0.98f
#define MaxPowerRatio 0.8f
#define BoostWindowNorm (MaxPowerRatio / 95.0f)
#define EPSILON 0.01f

// --- File Scope Static Data ---
// The boost window is constant based on FFT size, so we calculate it once globally.
static float __boost_window[FFTBins];
static bool __boost_initialized = false;

// --- Internal State Structure ---
typedef struct iq_state_s {
    // Shared State (Protected by Mutex)
    _Atomic uint64_t packed_state;

    // Apply-Only State (Accessed only by DSP thread, no lock needed)
    float last_phase;
    float last_amplitude;

    // Optimizer-Only State (Accessed only by Optimizer thread, no lock needed)
    // Changed accumulators to double to prevent precision drift
    double integrated_total_power;
    double integrated_image_power;
    float maximum_image_power;

    float raw_phases[MaxLookback];
    float raw_amplitudes[MaxLookback];

    int fft_integration;
    int fft_overlap;
    int correlation_integration;

    int no_of_avg;
    int no_of_raw;
    int raw_ptr;
    int optimal_bin;
    int reset_flag;
    int *power_flag;

    // Buffers (Optimizer Only)
    complex float *corr;
    complex float *corr_plus;
    float *boost;

    // Liquid-DSP Resources (Optimizer Only)
    fftplan fft_plan;
    complex float *fft_buffer; // Persistent buffer bound to the plan
    float *window_func;        // Pre-calculated window function
} IqState;

// --- Forward Declarations ---

static inline uint64_t pack_iq_state(float phase, float amplitude) {
    uint32_t pi, ai;
    memcpy(&pi, &phase, sizeof(float));
    memcpy(&ai, &amplitude, sizeof(float));
    return ((uint64_t)pi << 32) | ai;
}

static inline void unpack_iq_state(uint64_t packed, float* phase, float* amplitude) {
    uint32_t pi = (uint32_t)(packed >> 32);
    uint32_t ai = (uint32_t)(packed & 0xFFFFFFFF);
    memcpy(phase, &pi, sizeof(float));
    memcpy(amplitude, &ai, sizeof(float));
}

static void init_window(float * restrict w, int length);
static void init_boost_window(void);
static void apply_window(complex float * restrict buffer, float * restrict w, int length);
static float adjust_benchmark(complex float *iq, float phase, float amplitude);

// Updated signature to accept current values and return new values via pointers.
// This allows the calculation to happen without holding the main mutex.
static void estimate_imbalance(IqState *st, const complex float* restrict iq, int length,
                               float current_phase, float current_amp,
                               float* out_phase, float* out_amp);

// ============================================================================
// == Public API Implementation
// ============================================================================

bool iq_correction_init(AppConfig* config, AppContext* app, MemoryArena* arena) {
    if (!config->dsp.iq_correction.enable) {
        app->dsp.iq_correct.internal_state = NULL;
        return true;
    }

    // Allocate internal state
    IqState* st = (IqState*)mem_arena_alloc(arena, sizeof(IqState), true);
    if (!st) return false;

    // Default configuration
    st->optimal_bin = FFTBins / 2;

    // NOTE: In the original library, total integration = FFTIntegration * CorrelationIntegration.
    // Original: 4 * 16 = 64 frames.
    // Our pipeline processes 1 frame per call. To maintain the same statistical weight,
    // we set correlation_integration to 32 to get updates roughly every 0.5-1.0 seconds.
    st->fft_integration = 4; // Used only for sizing power_flag array in this adaptation
    st->fft_overlap = 2;
    st->correlation_integration = 32; // Reduced from 64 for faster convergence
    st->reset_flag = 1;

    // Allocate Buffers
    st->corr = (complex float*)mem_arena_alloc(arena, FFTBins * sizeof(complex float), true);
    st->corr_plus = (complex float*)mem_arena_alloc(arena, FFTBins * sizeof(complex float), true);
    st->boost = (float*)mem_arena_alloc(arena, FFTBins * sizeof(float), true);
    st->power_flag = (int*)mem_arena_alloc(arena, st->fft_integration * sizeof(int), true);

    // Allocate app for Liquid-DSP FFT
    st->fft_buffer = (complex float*)mem_arena_alloc(arena, FFTBins * sizeof(complex float), false);
    st->window_func = (float*)mem_arena_alloc(arena, FFTBins * sizeof(float), false);

    if (!st->corr || !st->corr_plus || !st->boost || !st->fft_buffer || !st->window_func) {
        return false;
    }

    // Initialize the window function (Blackman-Nuttall)
    init_window(st->window_func, FFTBins);

    // Initialize the global boost window if not already done
    if (!__boost_initialized) {
        init_boost_window();
        __boost_initialized = true;
    }

    // Create Liquid-DSP Plan (In-Place Forward FFT)
    st->fft_plan = fft_create_plan(
        FFTBins,
        (liquid_float_complex*)st->fft_buffer,
        (liquid_float_complex*)st->fft_buffer,
        LIQUID_FFT_FORWARD,
        0
    );

    if (!st->fft_plan) {
        log_fatal("Failed to create liquid-dsp FFT plan for I/Q correction.");
        return false;
    }

    app->dsp.iq_correct.internal_state = st;
    app->dsp.iq_correct.last_optimization_time = 0.0;

    log_info("I/Q Correction Enabled");
    return true;
}

void iq_correction_apply(DspContext* dsp, ComplexFloat* samples, int num_samples) {
    if (!dsp->config->dsp.iq_correction.enable || !dsp->iq_correct.internal_state) return;

    IqState* st = (IqState*)dsp->iq_correct.internal_state;

    // Lock briefly to read shared values.
    // This allows the optimizer to update them safely without tearing.
    uint64_t packed = atomic_load_explicit(&st->packed_state, memory_order_relaxed);
    float current_phase, current_amp;
    unpack_iq_state(packed, &current_phase, &current_amp);

    float scale = 1.0f / (num_samples - 1);
    float last_phase = st->last_phase;
    float last_amp = st->last_amplitude;

    for (int i = 0; i < num_samples; i++)
    {
        // Interpolate parameters across the buffer to prevent phase discontinuities
        float phase = (i * last_phase + (num_samples - 1 - i) * current_phase) * scale;
        float amplitude = (i * last_amp + (num_samples - 1 - i) * current_amp) * scale;

        float re = crealf(samples[i]);
        float im = cimagf(samples[i]);

        float new_re = re + phase * im;
        float new_im = im + phase * re;

        new_re *= (1.0f + amplitude);
        new_im *= (1.0f - amplitude);

        samples[i] = new_re + I * new_im;
    }

    st->last_phase = current_phase;
    st->last_amplitude = current_amp;
}

void iq_correction_run_estimation(DspContext* dsp, const ComplexFloat* optimization_data) {
    if (!dsp->config->dsp.iq_correction.enable || !dsp->iq_correct.internal_state) return;

    atomic_store_explicit(&dsp->iq_correct.last_optimization_time, utility_get_time(), memory_order_relaxed);
    IqState* st = (IqState*)dsp->iq_correct.internal_state;

    // Snapshot current values lock-free.
    uint64_t packed_start = atomic_load_explicit(&st->packed_state, memory_order_relaxed);
    float start_phase, start_amp;
    unpack_iq_state(packed_start, &start_phase, &start_amp);

    // Perform heavy calculation UNLOCKED using local variables.
    // This prevents stalling the high-priority DSP thread.
    float new_phase = start_phase;
    float new_amp = start_amp;

    estimate_imbalance(st, optimization_data, FFTBins, start_phase, start_amp, &new_phase, &new_amp);

    // Lock-free commit.
    uint64_t new_packed = pack_iq_state(new_phase, new_amp);
    atomic_store_explicit(&st->packed_state, new_packed, memory_order_relaxed);

    // Debug logging (rate limited)
    static double last_debug_log_time = 0.0;
    double current_opt_time = atomic_load_explicit(&dsp->iq_correct.last_optimization_time, memory_order_relaxed);
    if (current_opt_time - last_debug_log_time >= CONSOLE_UPDATE_INTERVAL_SEC) {
        float phase_deg = new_phase * (180.0f / (float)M_PI);
        float amp_pct = new_amp * 100.0f;
        float image_db = 10.0f * log10f((float)st->integrated_image_power + 1e-12f);

        log_debug("IQ Correct: Phase: %+.3f deg | Amp: %+.3f %% | Image Pwr: %.1f dBFS",
                 phase_deg, amp_pct, image_db);
        last_debug_log_time = current_opt_time;
    }
}

void iq_correction_destroy(AppContext* app) {
    if (app->dsp.iq_correct.internal_state) {
        IqState* st = (IqState*)app->dsp.iq_correct.internal_state;
        if (st->fft_plan) fft_destroy_plan(st->fft_plan);
        // Arena handles memory free
        app->dsp.iq_correct.internal_state = NULL;
    }
}

bool iq_correction_run_initial_calibration(ModuleContext* context, void* raw_buffer, size_t num_bytes,
                                           size_t (*read_cb)(void* user_data, void* buffer, size_t bytes), void* user_data) {
    AppContext* app = context->app;

    if (!raw_buffer || num_bytes == 0 || !read_cb) {
        log_warn("Cannot perform initial I/Q correction without valid input data or reader.");
        return true;
    }

    log_info("Performing initial I/Q calibration for file input...");

    ComplexFloat* cf32_buffer = (ComplexFloat*)mem_arena_alloc(&app->pipeline.setup_arena, IQ_CORRECTION_FFT_SIZE * sizeof(ComplexFloat), false);
    if (!cf32_buffer) {
        log_fatal("Failed to allocate temporary buffer for I/Q calibration.");
        return false;
    }

    SampleChunk temp_chunk;
    memset(&temp_chunk, 0, sizeof(SampleChunk));
    temp_chunk.raw_input_data = (void*)raw_buffer;
    temp_chunk.packet_sample_format = app->module.input_format;
    temp_chunk.pre_resample_buffer = cf32_buffer;

    for(int i=0; i<64; i++) {
        size_t bytes_read = read_cb(user_data, raw_buffer, num_bytes);
        if (bytes_read < num_bytes) break;

        temp_chunk.frames_read = IQ_CORRECTION_FFT_SIZE;
        pre_processor_apply_chain(&app->dsp, &temp_chunk);

        iq_correction_run_estimation(&app->dsp, temp_chunk.pre_resample_buffer);
        app->dsp.iq_correct.last_optimization_time = 0.0;
    }

    log_info("Initial I/Q calibration complete.");
    return true;
}

// ============================================================================
// == Internal Logic (Adapted from libairspyhf)
// ============================================================================

static void init_window(float * restrict w, int length)
{
    const int len_m1 = length - 1;
    for (int i = 0; i < length; i++)
    {
        w[i] = (float)(
            +0.35875f
            - 0.48829f * cos(2.0 * M_PI * i / len_m1)
            + 0.14128f * cos(4.0 * M_PI * i / len_m1)
            - 0.01168f * cos(6.0 * M_PI * i / len_m1)
            );
    }
}

static void init_boost_window(void) {
    for(int k=0; k<FFTBins; k++) {
        __boost_window[k] = (float)(1.0 / BoostFactor + 1.0 / exp(pow(k * 2.0 / BinsToOptimize, 2.0)));
    }
}

static void apply_window(complex float * restrict buffer, float * restrict w, int length)
{
    for (int i = 0; i < length; i++) {
        buffer[i] *= w[i];
    }
}

static float adjust_benchmark(complex float *iq, float phase, float amplitude)
{
    float sum = 0;
    for (int i = 0; i < FFTBins; i++)
    {
        float re = crealf(iq[i]);
        float im = cimagf(iq[i]);

        float new_re = re + phase * im;
        float new_im = im + phase * re;

        new_re *= (1.0f + amplitude);
        new_im *= (1.0f - amplitude);

        // Update buffer in-place for subsequent FFT
        iq[i] = new_re + I * new_im;

        sum += new_re * new_re + new_im * new_im;
    }
    return sum;
}

static complex float utility(IqState *st, complex float* ccorr)
{
    int i, j;
    float invskip = 1.0f / EdgeBinsToSkip;
    complex float acc = 0;

    // Uses the file-scope static __boost_window

    for (i = EdgeBinsToSkip, j = FFTBins - EdgeBinsToSkip; i <= FFTBins - EdgeBinsToSkip; i++, j--)
    {
        int distance = abs(i - FFTBins / 2);
        if (distance > CenterBinsToSkip)
        {
            float weight = (distance > EdgeBinsToSkip) ? 1.0f : (distance * invskip);
            if (st->optimal_bin != FFTBins / 2)
            {
                weight *= __boost_window[abs(st->optimal_bin - i)];
            }
            weight *= st->boost[j] / (st->boost[i] + EPSILON);
            acc += ccorr[i] * weight;
        }
    }
    return acc;
}

// Updated signature to accept params and return results via pointers.
// This decouples calculation from shared state.
static void estimate_imbalance(IqState *st, const complex float* restrict iq, int length,
                               float current_phase, float current_amp,
                               float* out_phase, float* out_amp)
{
    // Initialize outputs with current values in case we exit early
    *out_phase = current_phase;
    *out_amp = current_amp;

    int i, j;
    float amplitude, phase, mu;
    complex float a, b;

    if (st->reset_flag)
    {
        st->reset_flag = 0;
        st->no_of_avg = -BuffersToSkipOnReset;
        st->maximum_image_power = 0;
    }

    if (st->no_of_avg < 0)
    {
        st->no_of_avg++;
        return;
    }
    else if (st->no_of_avg == 0)
    {
        st->integrated_image_power = 0;
        st->integrated_total_power = 0;
        memset(st->boost, 0, FFTBins * sizeof(float));
        memset(st->corr, 0, FFTBins * sizeof(complex float));
        memset(st->corr_plus, 0, FFTBins * sizeof(complex float));
    }

    st->maximum_image_power *= MaxPowerDecay;

    // Use the persistent buffer for FFT operations to avoid stack thrashing
    // Safe because this function is only called from one thread (Optimizer)
    complex float *fftPtr = st->fft_buffer;

    // 1. Compute Correlation (Current Parameters)
    int count = 0;

    // Note: Original code handled sliding windows. Since we process fixed chunks
    // from the pipeline, we process one FFT frame here.
    if (length >= FFTBins)
    {
        memcpy(fftPtr, iq, FFTBins * sizeof(complex float));

        // Use passed-in values, not st->phase/amp
        float power = adjust_benchmark(fftPtr, current_phase, current_amp);

        if (power > MinimumPower)
        {
            apply_window(fftPtr, st->window_func, FFTBins);

            // Execute Liquid-DSP FFT (In-Place)
            fft_execute(st->fft_plan);

            for (i = EdgeBinsToSkip, j = FFTBins - EdgeBinsToSkip; i <= FFTBins - EdgeBinsToSkip; i++, j--)
            {
                st->corr[i] += fftPtr[i] * fftPtr[j];
                st->corr[j] = st->corr[i];
            }

            // Calculate boost (spectral power density)
            for (i = EdgeBinsToSkip; i <= FFTBins - EdgeBinsToSkip; i++)
            {
                float bin_power = crealf(fftPtr[i])*crealf(fftPtr[i]) + cimagf(fftPtr[i])*cimagf(fftPtr[i]);
                st->boost[i] += bin_power;
                if (st->optimal_bin == FFTBins/2)
                    st->integrated_image_power += bin_power;
                else
                    st->integrated_image_power += bin_power * __boost_window[abs(FFTBins - i - st->optimal_bin)];
            }
            st->integrated_total_power += power;
            count++;
        }
    }

    if (count == 0) return;
    st->no_of_avg += count;

    // 2. Compute Correlation (Perturbed Parameters) - Re-use buffer
    memcpy(fftPtr, iq, FFTBins * sizeof(complex float));
    // Use passed-in values for perturbation base
    adjust_benchmark(fftPtr, current_phase + PhaseStep, current_amp + AmplitudeStep);
    apply_window(fftPtr, st->window_func, FFTBins);
    fft_execute(st->fft_plan);

    for (i = EdgeBinsToSkip, j = FFTBins - EdgeBinsToSkip; i <= FFTBins - EdgeBinsToSkip; i++, j--)
    {
        st->corr_plus[i] += fftPtr[i] * fftPtr[j];
        st->corr_plus[j] = st->corr_plus[i];
    }

    // Check integration limit
    if (st->no_of_avg <= st->correlation_integration) return;
    st->no_of_avg = 0;

    // Power Threshold Check
    if (st->optimal_bin == FFTBins / 2)
    {
        if (st->integrated_total_power < st->maximum_image_power) return;
        st->maximum_image_power = (float)st->integrated_total_power;
    }
    else
    {
        if (st->integrated_image_power - st->integrated_total_power * BoostWindowNorm < st->maximum_image_power * PowerThreshold)
            return;
        st->maximum_image_power = (float)(st->integrated_image_power - st->integrated_total_power * BoostWindowNorm);
    }

    // Calculate utility vectors
    a = utility(st, st->corr);
    b = utility(st, st->corr_plus);

    // Update Phase
    mu = cimagf(a) - cimagf(b);
    if (fabs(mu) > MinDeltaMu)
    {
        mu = cimagf(a) / mu;
        if (mu < -MaxMu) mu = -MaxMu;
        else if (mu > MaxMu) mu = MaxMu;
    }
    else
    {
        mu = 0;
    }
    phase = current_phase + PhaseStep * mu;

    // Update Amplitude
    mu = crealf(a) - crealf(b);
    if (fabs(mu) > MinDeltaMu)
    {
        mu = crealf(a) / mu;
        if (mu < -MaxMu) mu = -MaxMu;
        else if (mu > MaxMu) mu = MaxMu;
    }
    else
    {
        mu = 0;
    }
    amplitude = current_amp + AmplitudeStep * mu;

    // History Smoothing
    if (st->no_of_raw < MaxLookback)
        st->no_of_raw++;

    st->raw_amplitudes[st->raw_ptr] = amplitude;
    st->raw_phases[st->raw_ptr] = phase;

    i = st->raw_ptr;
    // Note: Reset accumulators for smoothing calc
    phase = 0;
    amplitude = 0;

    for (j = 0; j < st->no_of_raw; j++) // Correction: Loop over valid raw entries
    {
        int index = (st->raw_ptr + MaxLookback - j) & (MaxLookback - 1);
        phase += st->raw_phases[index];
        amplitude += st->raw_amplitudes[index];
    }
    phase /= st->no_of_raw;
    amplitude /= st->no_of_raw;
    st->raw_ptr = (st->raw_ptr + 1) & (MaxLookback - 1);

    // Commit new parameters to output pointers
    *out_phase = phase;
    *out_amp = amplitude;
}
