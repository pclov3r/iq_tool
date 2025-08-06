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
 *  --- Code Provenance and Attribution ---
 *
 *  The I/Q correction algorithm in this file is a derivative work, adapted
 *  from 'iqbal.c' in the libosmo-dsp project by Sylvain Munaut.
 *
 *  The following descriptive block is adapted from the original 'iqbal.c'
 *  header to preserve its attribution and context:
 *
 *  "The actual algorithm used for estimation of the imbalance and its
 *   optimization is inspired by the IQ balancer of SDR# by Youssef Touil
 *   and described here :
 *
 *   https://web.archive.org/web/20140209125600/http://sdrsharp.com/index.php/automatic-iq-correction-algorithm
 *
 *   The main differences are:
 *    - Objective function uses complex correlation of left/right side of FFT
 *    - Optimization based on steepest gradient with dynamic step size"
 *
 *  Further modifications in this file include the use of liquid-dsp for
 *  FFT operations and adaptation for continuous stream processing.
 *
 *  The original source repository for libosmo-dsp is:
 *      https://github.com/osmocom/libosmo-dsp
 *
 *  The original copyright and license notice from 'iqbal.c' is preserved
 *  below as required by its license.
 */

/*
 * Copyright (C) 2013  Sylvain Munaut <tnt@246tNt.com>
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 2110-1301 USA.
 */

#include "iq_correct.h"
#include "log.h"
#include "config.h" // For IQ_CORRECTION_FFT_SIZE, IQ_CORRECTION_FFT_COUNT, etc.
#include <stdlib.h>
#include <string.h>
#include <math.h>   // For M_PI, fabs, isfinite
#include <errno.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

// --- Internal Helper Functions (derived from libosmo-dsp's iqbal.c) ---

static inline float _osmo_normsqf(complex_float_t x) {
    return crealf(x)*crealf(x) + cimagf(x)*cimagf(x);
}

static float _iq_balance_calculate_estimate(const complex_float_t *data, int fft_size, int fft_count, fftplan plan, complex_float_t* fft_input_buffer, complex_float_t* fft_output_buffer) {
    float est = 0.0f;
    int i, j;

    for (i = 0; i < fft_count; i++) {
        complex_float_t corr = 0.0f;
        memcpy(fft_input_buffer, &data[i * fft_size], sizeof(complex_float_t) * fft_size);
        fft_execute(plan);
        for (j = 1; j < fft_size / 2; j++) {
            corr += fft_output_buffer[fft_size - j] * conjf(fft_output_buffer[j]);
        }
        est += _osmo_normsqf(corr);
    }
    return est;
}

static inline float _iqbal_objfn_value(IqCorrectionResources* iq_res, const complex_float_t* original_signal, int signal_len, float x[2]) {
    const float magp1 = 1.0f + x[0];
    const float phase = x[1];

    for (int i = 0; i < signal_len; ++i) {
        complex_float_t v = original_signal[i];
        iq_res->tmp_signal_buffer[i] = (crealf(v) * magp1) + (cimagf(v) + phase * crealf(v)) * I;
    }
    return _iq_balance_calculate_estimate(iq_res->tmp_signal_buffer, IQ_CORRECTION_FFT_SIZE, IQ_CORRECTION_FFT_COUNT, iq_res->fft_plan, iq_res->fft_input_buffer, iq_res->fft_output_buffer);
}

static void _iqbal_objfn_gradient(IqCorrectionResources* iq_res, const complex_float_t* original_signal, int signal_len, float x[2], float v, float grad[2]) {
    const float GRAD_STEP = 1e-6f;
    float xd[2], vd[2];

    xd[0] = x[0] + GRAD_STEP; xd[1] = x[1];
    vd[0] = _iqbal_objfn_value(iq_res, original_signal, signal_len, xd);

    xd[0] = x[0]; xd[1] = x[1] + GRAD_STEP;
    vd[1] = _iqbal_objfn_value(iq_res, original_signal, signal_len, xd);

    grad[0] = (vd[0] - v) / GRAD_STEP;
    grad[1] = (vd[1] - v) / GRAD_STEP;
}

static inline float _iqbal_objfn_val_gradient(IqCorrectionResources* iq_res, const complex_float_t* original_signal, int signal_len, float x[2], float grad[2]) {
    float v = _iqbal_objfn_value(iq_res, original_signal, signal_len, x);
    _iqbal_objfn_gradient(iq_res, original_signal, signal_len, x, v, grad);
    return v;
}

bool iq_correct_init(AppConfig* config, AppResources* resources) {
    if (!config->iq_correction.enable) {
        resources->iq_correction.fft_plan = NULL;
        resources->iq_correction.fft_input_buffer = NULL;
        resources->iq_correction.fft_output_buffer = NULL;
        resources->iq_correction.tmp_signal_buffer = NULL;
        resources->iq_correction.optimization_accum_buffer = NULL;
        return true;
    }

    const unsigned int nfft = IQ_CORRECTION_FFT_SIZE;
    const unsigned int total_samples_for_optimize = IQ_CORRECTION_FFT_SIZE * IQ_CORRECTION_FFT_COUNT;

    resources->iq_correction.fft_input_buffer = (complex_float_t*)fft_malloc(nfft * sizeof(complex_float_t));
    resources->iq_correction.fft_output_buffer = (complex_float_t*)fft_malloc(nfft * sizeof(complex_float_t));
    resources->iq_correction.tmp_signal_buffer = (complex_float_t*)malloc(total_samples_for_optimize * sizeof(complex_float_t));

    // The accumulation buffer must be large enough to hold the default period
    resources->iq_correction.optimization_accum_buffer = (complex_float_t*)malloc(IQ_CORRECTION_DEFAULT_PERIOD * sizeof(complex_float_t));

    if (!resources->iq_correction.fft_input_buffer || !resources->iq_correction.fft_output_buffer ||
        !resources->iq_correction.tmp_signal_buffer || !resources->iq_correction.optimization_accum_buffer) {
        log_fatal("Failed to allocate memory for I/Q correction buffers: %s", strerror(errno));
        iq_correct_cleanup(resources);
        return false;
    }

    resources->iq_correction.fft_plan = fft_create_plan(nfft, resources->iq_correction.fft_input_buffer, resources->iq_correction.fft_output_buffer, LIQUID_FFT_FORWARD, 0);
    if (!resources->iq_correction.fft_plan) {
        log_fatal("Failed to create liquid-dsp FFT plan for I/Q correction.");
        iq_correct_cleanup(resources);
        return false;
    }

    // Initialize correction parameters to 0.0f, representing "no correction".
    // This is a fundamental constant, not a configurable "magic number".
    resources->iq_correction.current_mag = 0.0f;
    resources->iq_correction.current_phase = 0.0f;
    resources->iq_correction.samples_accumulated_for_optimize = 0;

    log_info("I/Q Correction enabled (using default internal parameters).");

    return true;
}

void iq_correct_apply(AppResources* resources, complex_float_t* samples, int num_samples) {
    if (!resources->config->iq_correction.enable) {
        return;
    }

    const float magp1 = 1.0f + resources->iq_correction.current_mag;
    const float phase = resources->iq_correction.current_phase;

    // Optimization: if correction is negligible, skip the loop.
    if (fabs(resources->iq_correction.current_mag) < 1e-6f && fabs(resources->iq_correction.current_phase) < 1e-6f) {
        return;
    }

    for (int i = 0; i < num_samples; i++) {
        complex_float_t v = samples[i];
        samples[i] = (crealf(v) * magp1) + (cimagf(v) + phase * crealf(v)) * I;
    }
}

void iq_correct_run_optimization(AppResources* resources, const complex_float_t* optimization_data, int num_optimization_samples) {
    if (!resources->config->iq_correction.enable) {
        return;
    }

    if (num_optimization_samples < (IQ_CORRECTION_FFT_SIZE * IQ_CORRECTION_FFT_COUNT)) {
        log_warn("IQ_OPT: Not enough samples provided (%d) to run optimization. Required: %d.",
                 num_optimization_samples, (IQ_CORRECTION_FFT_SIZE * IQ_CORRECTION_FFT_COUNT));
        return;
    }

    float current_params[2];
    float grad[2];
    float cv, nv, step;
    float nx[2];
    float p;
    int i;

    current_params[0] = resources->iq_correction.current_mag;
    current_params[1] = resources->iq_correction.current_phase;

    cv = _iqbal_objfn_val_gradient(&resources->iq_correction, optimization_data, num_optimization_samples, current_params, grad);

    const float EPSILON = 1e-12f;
    float grad_mag_sum = fabs(grad[0]) + fabs(grad[1]);
    step = cv / (grad_mag_sum + EPSILON);

    log_debug("IQ_OPT: Initial: mag=%.4f, phase=%.4f, cv=%.2e, grad=[%.2e,%.2e], step=%.2e",
              current_params[0], current_params[1], cv, grad[0], grad[1], step);

    for (i = 0; i < IQ_CORRECTION_MAX_ITER; i++) {
        float grad_norm_denom = (fabs(grad[0]) + fabs(grad[1])) + EPSILON;
        nx[0] = current_params[0] - step * (grad[0] / grad_norm_denom);
        nx[1] = current_params[1] - step * (grad[1] / grad_norm_denom);

        nv = _iqbal_objfn_value(&resources->iq_correction, optimization_data, num_optimization_samples, nx);

        if (!isfinite(nv)) {
            log_error("IQ_OPT: Non-finite value (NaN/Inf) detected for nv at iteration %d. Stopping optimization.", i);
            break;
        }

        if (nv <= cv) {
            p = (cv - nv) / (cv + EPSILON);
            current_params[0] = nx[0];
            current_params[1] = nx[1];
            cv = nv;
            _iqbal_objfn_gradient(&resources->iq_correction, optimization_data, num_optimization_samples, current_params, cv, grad);

            if (!isfinite(grad[0]) || !isfinite(grad[1])) {
                log_error("IQ_OPT: Non-finite gradient detected at iteration %d. Stopping optimization.", i);
                break;
            }

            if (p < 0.01f) {
                break;
            }
        } else {
            step /= 2.0 * (nv / (cv + EPSILON));
        }
    }

    resources->iq_correction.current_mag = (0.95f * resources->iq_correction.current_mag) + (current_params[0] * 0.05f);
    resources->iq_correction.current_phase = (0.95f * resources->iq_correction.current_phase) + (current_params[1] * 0.05f);

}

void iq_correct_cleanup(AppResources* resources) {
    if (resources->iq_correction.fft_plan) {
        fft_destroy_plan(resources->iq_correction.fft_plan);
        resources->iq_correction.fft_plan = NULL;
    }
    if (resources->iq_correction.fft_input_buffer) {
        fft_free(resources->iq_correction.fft_input_buffer);
        resources->iq_correction.fft_input_buffer = NULL;
    }
    if (resources->iq_correction.fft_output_buffer) {
        fft_free(resources->iq_correction.fft_output_buffer);
        resources->iq_correction.fft_output_buffer = NULL;
    }
    free(resources->iq_correction.tmp_signal_buffer);
    resources->iq_correction.tmp_signal_buffer = NULL;
    free(resources->iq_correction.optimization_accum_buffer);
    resources->iq_correction.optimization_accum_buffer = NULL;
}
