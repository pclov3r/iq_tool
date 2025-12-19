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

#include "iq_correct.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "memory_arena.h"
#include "utils.h"
#include "pre_processor.h" // Needed for initial calibration chain
#include "sample_convert.h" // Needed for byte size calculations
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
typedef struct {
    float phase;
    float last_phase;

    float amplitude;
    float last_amplitude;

    float integrated_total_power;
    float integrated_image_power;
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

    // Buffers
    complex float *corr;
    complex float *corr_plus;
    float *boost;

    // Liquid-DSP Resources
    fftplan fft_plan;
    complex float *fft_buffer; // Persistent buffer bound to the plan
    float *window_func;        // Pre-calculated window function
} IqState;

// --- Forward Declarations ---
static void init_window(float * restrict w, int length);
static void init_boost_window(void);
static void apply_window(complex float * restrict buffer, float * restrict w, int length);
static void adjust_phase_amplitude(IqState *st, complex float* restrict iq, int length);
static void estimate_imbalance(IqState *st, const complex float* restrict iq, int length);

// ============================================================================
// == Public API Implementation
// ============================================================================

bool iq_correct_init(AppConfig* config, AppContext* app, MemoryArena* arena) {
    if (!config->dsp.iq_correction.enable) {
        app->dsp.iq_correct.internal_state = NULL;
        return true;
    }

    if (pthread_mutex_init(&app->dsp.iq_correct.iq_factors_mutex, NULL) != 0) {
        log_fatal("Failed to initialize I/Q correction mutex.");
        return false;
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

void iq_correct_apply(DspContext* dsp, complex_float_t* samples, int num_samples) {
    if (!dsp->config->dsp.iq_correction.enable || !dsp->iq_correct.internal_state) return;

    IqState* st = (IqState*)dsp->iq_correct.internal_state;

    // Thread Safety: Lock to ensure consistent reads of phase/amplitude parameters.
    // The optimizer thread may update these values, so we must synchronize access.
    // Lock contention is rare (optimizer runs infrequently) and hold time is minimal.
    pthread_mutex_lock(&dsp->iq_correct.iq_factors_mutex);
    adjust_phase_amplitude(st, samples, num_samples);
    pthread_mutex_unlock(&dsp->iq_correct.iq_factors_mutex);
}

void iq_correct_run_optimization(DspContext* dsp, const complex_float_t* optimization_data) {
    if (!dsp->config->dsp.iq_correction.enable || !dsp->iq_correct.internal_state) return;

    // RATE LIMITER REMOVED:
    // We want the optimizer to process every chunk available in the queue to ensure
    // fast convergence. The thread priority (LOW) ensures it doesn't starve the audio.

    // Update timestamp just for logging purposes
    double current_time = get_monotonic_time_sec();
    dsp->iq_correct.last_optimization_time = current_time;

    IqState* st = (IqState*)dsp->iq_correct.internal_state;

    // The algorithm requires at least one FFT frame.
    // The pipeline chunk is typically larger (~12k samples), so this is safe.
    pthread_mutex_lock(&dsp->iq_correct.iq_factors_mutex);
    estimate_imbalance(st, optimization_data, FFTBins);

    // --- DEBUG LOGGING ---
    // Log once every 1.0 seconds to avoid console spam, but process continuously.
    static double last_debug_log_time = 0.0;

    if (current_time - last_debug_log_time >= 1.0) {
        float phase_deg = st->phase * (180.0f / (float)M_PI);
        float amp_pct = st->amplitude * 100.0f;
        // Simple dB calculation of the integration metric
        float image_db = 10.0f * log10f(st->integrated_image_power + 1e-12f);

        log_debug("IQ Correction: Phase: %+.3f deg | Amp: %+.3f %% | Image Pwr: %.1f dB",
                 phase_deg, amp_pct, image_db);
        last_debug_log_time = current_time;
    }
    // -----------------------------------------

    pthread_mutex_unlock(&dsp->iq_correct.iq_factors_mutex);
}

void iq_correct_destroy(AppContext* app) {
    if (app->dsp.iq_correct.internal_state) {
        IqState* st = (IqState*)app->dsp.iq_correct.internal_state;
        if (st->fft_plan) fft_destroy_plan(st->fft_plan);
        // Arena handles memory free
        app->dsp.iq_correct.internal_state = NULL;
    }
    pthread_mutex_destroy(&app->dsp.iq_correct.iq_factors_mutex);
}

bool iq_correct_run_initial_calibration(ModuleContext* ctx, SNDFILE* infile) {
    AppContext* app = ctx->app;

    if (!infile) {
        log_warn("Cannot perform initial I/Q correction without a valid file handle.");
        return true;
    }

    log_info("Performing initial I/Q calibration for file input...");

    if (app->module.source_info.frames < FFTBins) {
        log_warn("Input file is too short for I/Q calibration. Skipping.");
        return true;
    }

    // Allocate temporary buffers from the setup arena.
    size_t raw_buffer_size = FFTBins * app->module.input_bytes_per_sample_pair;
    void* raw_buffer = mem_arena_alloc(&app->pipeline.setup_arena, raw_buffer_size, false);
    complex_float_t* cf32_buffer = (complex_float_t*)mem_arena_alloc(&app->pipeline.setup_arena, FFTBins * sizeof(complex_float_t), false);

    if (!raw_buffer || !cf32_buffer) {
        log_fatal("Failed to allocate temporary buffers for I/Q calibration.");
        return false;
    }

    // Read the first block of samples.
    sf_count_t frames_read_bytes = sf_read_raw(infile, raw_buffer, raw_buffer_size);
    if (frames_read_bytes < (sf_count_t)raw_buffer_size) {
        log_warn("Failed to read enough samples for I/Q calibration. Skipping.");
        sf_seek(infile, 0, SEEK_SET);
        return true;
    }

    // Create a temporary SampleChunk to pass to the pre-processor chain.
    SampleChunk temp_chunk;
    memset(&temp_chunk, 0, sizeof(SampleChunk));
    temp_chunk.raw_input_data = raw_buffer;
    temp_chunk.frames_read = FFTBins;
    temp_chunk.packet_sample_format = app->module.input_format;
    temp_chunk.complex_sample_buffer_a = cf32_buffer;
    temp_chunk.current_output_buffer = temp_chunk.complex_sample_buffer_a;

    // Run the pre-processor (Format conversion + DC Block + IQ Apply)
    // NOTE: This applies current (0,0) correction, but importantly removes DC.
    pre_processor_apply_chain(&app->dsp, &temp_chunk);

    // Run the optimization algorithm synchronously on the processed data multiple times to converge
    for(int i=0; i<64; i++) {
        iq_correct_run_optimization(&app->dsp, temp_chunk.current_output_buffer);
        // Force timestamp update so the loop doesn't get rate-limited internally
        app->dsp.iq_correct.last_optimization_time = 0.0;
    }

    // Rewind the file so the main reader thread can process the file from the start.
    if (sf_seek(infile, 0, SEEK_SET) < 0) {
        log_fatal("Failed to rewind input file after I/Q calibration.");
        return false;
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

static void adjust_phase_amplitude(IqState *st, complex float* restrict iq, int length)
{
    float scale = 1.0f / (length - 1);
    float current_phase = st->phase;
    float current_amp = st->amplitude;
    float last_phase = st->last_phase;
    float last_amp = st->last_amplitude;

    for (int i = 0; i < length; i++)
    {
        // Interpolate parameters across the buffer to prevent phase discontinuities
        float phase = (i * last_phase + (length - 1 - i) * current_phase) * scale;
        float amplitude = (i * last_amp + (length - 1 - i) * current_amp) * scale;

        float re = crealf(iq[i]);
        float im = cimagf(iq[i]);

        float new_re = re + phase * im;
        float new_im = im + phase * re;

        new_re *= (1.0f + amplitude);
        new_im *= (1.0f - amplitude);

        iq[i] = new_re + I * new_im;
    }

    st->last_phase = current_phase;
    st->last_amplitude = current_amp;
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

static void estimate_imbalance(IqState *st, const complex float* restrict iq, int length)
{
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
    complex float *fftPtr = st->fft_buffer;

    // 1. Compute Correlation (Current Parameters)
    int count = 0;

    // Note: Original code handled sliding windows. Since we process fixed chunks
    // from the pipeline, we process one FFT frame here.
    if (length >= FFTBins)
    {
        memcpy(fftPtr, iq, FFTBins * sizeof(complex float));

        float power = adjust_benchmark(fftPtr, st->phase, st->amplitude);

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
                float p = crealf(fftPtr[i])*crealf(fftPtr[i]) + cimagf(fftPtr[i])*cimagf(fftPtr[i]);
                st->boost[i] += p;
                if (st->optimal_bin == FFTBins/2)
                    st->integrated_image_power += p;
                else
                    st->integrated_image_power += p * __boost_window[abs(FFTBins - i - st->optimal_bin)];
            }
            st->integrated_total_power += power;
            count++;
        }
    }

    if (count == 0) return;
    st->no_of_avg += count;

    // 2. Compute Correlation (Perturbed Parameters) - Re-use buffer
    memcpy(fftPtr, iq, FFTBins * sizeof(complex float));
    adjust_benchmark(fftPtr, st->phase + PhaseStep, st->amplitude + AmplitudeStep);
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
        st->maximum_image_power = st->integrated_total_power;
    }
    else
    {
        if (st->integrated_image_power - st->integrated_total_power * BoostWindowNorm < st->maximum_image_power * PowerThreshold)
            return;
        st->maximum_image_power = st->integrated_image_power - st->integrated_total_power * BoostWindowNorm;
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
    phase = st->phase + PhaseStep * mu;

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
    amplitude = st->amplitude + AmplitudeStep * mu;

    // History Smoothing
    if (st->no_of_raw < MaxLookback)
        st->no_of_raw++;

    st->raw_amplitudes[st->raw_ptr] = amplitude;
    st->raw_phases[st->raw_ptr] = phase;

    i = st->raw_ptr;
    for (j = 0; j < st->no_of_raw - 1; j++)
    {
        i = (i + MaxLookback - 1) & (MaxLookback - 1);
        phase += st->raw_phases[i];
        amplitude += st->raw_amplitudes[i];
    }
    phase /= st->no_of_raw;
    amplitude /= st->no_of_raw;
    st->raw_ptr = (st->raw_ptr + 1) & (MaxLookback - 1);

    // Commit new parameters
    st->phase = phase;
    st->amplitude = amplitude;
}
