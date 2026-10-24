/**
 * @file iq_correction.c
 * @brief Real-time automatic I/Q phase and amplitude imbalance correction.
 */

/*
 * Original Work Copyright (c) 2016-2023, Youssef Touil <youssef@airspy.com>
 * Original Work Copyright (c) 2018, Leif Asbrink <leif@sm5bsz.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
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
 * The modifications to this file are licensed under the GNU General Public
 * License v3.
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

#include "dsp/iq_correction.h"
#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "mem_arena.h"
#include "module.h"
#include "module_registry.h"
#include "platform.h"
#include "process_chain_context.h"
#include "process_chain_manager.h"
#include "queue.h"
#include "sample_conversion_functions.h"
#include "signal_handler.h"
#include "thread_manager.h"
#include "utilities.h"
#include <complex.h>
#include <liquid.h>
#include <math.h>
#include <stdatomic.h> // Needed for byte size calculations
#include <stdlib.h>
#include <string.h>

// clang-format off
// --- Default Configuration ---
#define IQ_FFT_BINS 4096

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Constants ---
#define IQ_BOOST_FACTOR 100000.0
#define IQ_BINS_TO_OPTIMIZE (IQ_FFT_BINS / 25)
#define IQ_EDGE_BINS_TO_SKIP (IQ_FFT_BINS / 22)
#define IQ_CENTER_BINS_TO_SKIP 2
#define IQ_MAX_LOOKBACK 4
#define IQ_PHASE_STEP 1e-2f
#define IQ_AMPLITUDE_STEP 1e-2f
#define IQ_MAX_MU 50.0f
#define IQ_MIN_DELTA_MU 0.1f
#define IQ_MINIMUM_POWER 0.01f
#define IQ_POWER_THRESHOLD 0.5f
#define IQ_BUFFERS_TO_SKIP_ON_RESET 2
#define IQ_MAX_POWER_DECAY 0.98f
#define IQ_MAX_POWER_RATIO 0.8f
#define IQ_BOOST_WINDOW_NORMALIZATION (IQ_MAX_POWER_RATIO / 95.0f)
#define IQ_EPSILON 0.01f
#define IQ_FFT_INTEGRATION 4
#define IQ_FFT_OVERLAP 2
#define IQ_CORRELATION_INTEGRATION 16
#define IQ_WORKING_BUFFER_LENGTH (IQ_FFT_BINS * (1 + IQ_FFT_INTEGRATION / IQ_FFT_OVERLAP))
// clang-format on

// --- File Scope Static Data ---
// The boost window is constant based on FFT size, so we calculate it once
// globally.
static float s_boost_window[IQ_FFT_BINS];
static bool s_boost_initialized = false;

// --- Internal State Structure ---
typedef struct iq_state_s {
  // Shared State (Lock-Free Atomic)
  _Atomic uint64_t packed_state;
  _Atomic double last_optimization_time;
  AppContext *app;

  // Apply-Only State (Accessed only by DSP thread, no lock needed)
  float last_phase;
  float last_amplitude;

  // Optimizer-Only State (Accessed only by Optimizer thread, no lock needed)
  // Changed accumulators to double to prevent precision drift
  double integrated_total_power;
  double integrated_image_power;
  float maximum_image_power;

  // DSP Thread State (for async queueing)
  ComplexFloat *current_estimation_buffer;
  int current_estimation_collected;

  float raw_phases[IQ_MAX_LOOKBACK];
  float raw_amplitudes[IQ_MAX_LOOKBACK];

  int fft_integration;
  int fft_overlap;
  int correlation_integration;

  int integration_count;
  int history_count;
  int history_index;
  int optimal_bin;
  int reset_flag;
  int *power_flags;

  Queue data_queue;
  Queue free_queue;

  // Buffers (Optimizer Only)
  complex float *correlation;
  complex float *correlation_perturbed;
  float *boost_spectrum;

  // Liquid-DSP Resources (Optimizer Only)
  fftplan fft_plan;
  complex float *fft_buffer; // Persistent buffer bound to the plan
  float *window_function;    // Pre-calculated window function
} IqState;

// --- Forward Declarations ---

static inline uint64_t pack_iq_state(float phase, float amplitude) {
  uint32_t phase_bits, amplitude_bits;
  memcpy(&phase_bits, &phase, sizeof(float));
  memcpy(&amplitude_bits, &amplitude, sizeof(float));
  return ((uint64_t)phase_bits << 32) | amplitude_bits;
}

static inline void unpack_iq_state(uint64_t packed, float *phase,
                                   float *amplitude) {
  uint32_t phase_bits = (uint32_t)(packed >> 32);
  uint32_t amplitude_bits = (uint32_t)(packed & 0xFFFFFFFF);
  memcpy(phase, &phase_bits, sizeof(float));
  memcpy(amplitude, &amplitude_bits, sizeof(float));
}

static void init_window(float *restrict window, int length);
static void init_boost_window(void);
static void apply_window(complex float *restrict buffer,
                         const float *restrict window, int length);
static float adjust_benchmark(complex float *iq, float phase, float amplitude,
                              int skip_power_calculation);
static void estimate_imbalance(IqState *state, const complex float *restrict iq,
                               int length, float current_phase,
                               float current_amplitude, float *out_phase,
                               float *out_amplitude);

// ============================================================================
// == Public API Implementation
// ============================================================================

static float s_calibrated_phase = 0.0f;
static float s_calibrated_amplitude = 0.0f;

static void *iq_correction_init(AppConfig *config, AppContext *app,
                                MemoryArena *arena) {
  if (!config->dsp.iq_correction.enable)
    return NULL;

  IqState *state = (IqState *)mem_arena_alloc(arena, sizeof(IqState), true);
  if (!state) {
    log_fatal("Failed to allocate memory for I/Q Correction state.");
    return NULL;
  }

  // Default configuration
  state->optimal_bin = IQ_FFT_BINS / 2;
  state->fft_integration = IQ_FFT_INTEGRATION;
  state->fft_overlap = IQ_FFT_OVERLAP;
  state->correlation_integration = IQ_CORRELATION_INTEGRATION;
  state->reset_flag = 1;

  // Allocate Buffers
  state->correlation = (complex float *)mem_arena_alloc(
      arena, IQ_FFT_BINS * sizeof(complex float), true);
  state->correlation_perturbed = (complex float *)mem_arena_alloc(
      arena, IQ_FFT_BINS * sizeof(complex float), true);
  state->boost_spectrum =
      (float *)mem_arena_alloc(arena, IQ_FFT_BINS * sizeof(float), true);
  state->power_flags =
      (int *)mem_arena_alloc(arena, state->fft_integration * sizeof(int), true);

  // Allocate buffers for Liquid-DSP FFT
  state->fft_buffer = (complex float *)mem_arena_alloc(
      arena, IQ_FFT_BINS * sizeof(complex float), false);
  state->window_function =
      (float *)mem_arena_alloc(arena, IQ_FFT_BINS * sizeof(float), false);

  if (!state->correlation || !state->correlation_perturbed ||
      !state->boost_spectrum || !state->power_flags || !state->fft_buffer ||
      !state->window_function) {
    return NULL;
  }

  // Initialize the window function (4-term Blackman-Harris)
  init_window(state->window_function, IQ_FFT_BINS);

  // Initialize the global boost window if not already done
  if (!s_boost_initialized) {
    init_boost_window();
    s_boost_initialized = true;
  }

  // Create Liquid-DSP Plan (In-Place Forward FFT)
  state->fft_plan = fft_create_plan(
      IQ_FFT_BINS, (liquid_float_complex *)state->fft_buffer,
      (liquid_float_complex *)state->fft_buffer, LIQUID_FFT_FORWARD, 0);

  if (!state->fft_plan) {
    log_fatal("Failed to create liquid-dsp FFT plan for I/Q correction.");
    return NULL;
  }

  atomic_store_explicit(&state->last_optimization_time, 0.0,
                        memory_order_relaxed);
  state->app = app;
  state->last_phase = s_calibrated_phase;
  state->last_amplitude = s_calibrated_amplitude;

  // Also pack the initial state into the atomic variable so other threads see
  // it
  uint64_t initial_packed =
      pack_iq_state(s_calibrated_phase, s_calibrated_amplitude);
  atomic_store_explicit(&state->packed_state, initial_packed,
                        memory_order_relaxed);

  if (!queue_init(&state->data_queue, 32, arena) ||
      !queue_init(&state->free_queue, 32, arena)) {
    log_fatal("Failed to initialize queues for I/Q Correction.");
    return NULL;
  }
  for (int i = 0; i < 16; i++) {
    void *buffer = mem_arena_alloc(
        arena, IQ_WORKING_BUFFER_LENGTH * sizeof(ComplexFloat), false);
    if (!buffer) {
      return NULL;
    }
    queue_enqueue(&state->free_queue, buffer);
  }

  log_info("I/Q Correction Enabled");
  return state;
}

static void iq_correction_apply(IqState *state, ComplexFloat *samples,
                                int num_samples) {
  if (!state || num_samples <= 0)
    return;

  // Lock briefly to read shared values.
  // This allows the optimizer to update them safely without tearing.
  uint64_t packed =
      atomic_load_explicit(&state->packed_state, memory_order_relaxed);
  float current_phase, current_amplitude;
  unpack_iq_state(packed, &current_phase, &current_amplitude);

  float scale = (num_samples > 1) ? (1.0f / (float)(num_samples - 1)) : 1.0f;
  float last_phase = state->last_phase;
  float last_amplitude = state->last_amplitude;

  for (int i = 0; i < num_samples; i++) {
    // Interpolate parameters across the buffer to prevent phase discontinuities
    float phase =
        (num_samples > 1)
            ? ((num_samples - 1 - i) * last_phase + i * current_phase) * scale
            : current_phase;
    float amplitude =
        (num_samples > 1)
            ? ((num_samples - 1 - i) * last_amplitude + i * current_amplitude) *
                  scale
            : current_amplitude;

    float real_part = crealf(samples[i]);
    float imag_part = cimagf(samples[i]);

    float new_real = real_part + phase * imag_part;
    float new_imag = imag_part + phase * real_part;

    new_real *= (1.0f + amplitude);
    new_imag *= (1.0f - amplitude);

    samples[i] = new_real + I * new_imag;
  }

  state->last_phase = current_phase;
  state->last_amplitude = current_amplitude;
}

void iq_correction_run_estimation(void *state,
                                  const ComplexFloat *optimization_data) {
  if (!state)
    return;

  IqState *iq_state = (IqState *)state;

  if (!iq_state->app->config->dsp.iq_correction.enable)
    return;

  atomic_store_explicit(&iq_state->last_optimization_time, platform_get_time(),
                        memory_order_relaxed);

  // Snapshot current values lock-free.
  uint64_t packed_start =
      atomic_load_explicit(&iq_state->packed_state, memory_order_relaxed);
  float start_phase, start_amplitude;
  unpack_iq_state(packed_start, &start_phase, &start_amplitude);

  // Perform heavy calculation UNLOCKED using local variables.
  // This prevents stalling the high-priority DSP thread.
  float new_phase = start_phase;
  float new_amplitude = start_amplitude;

  estimate_imbalance(iq_state, optimization_data, IQ_WORKING_BUFFER_LENGTH,
                     start_phase, start_amplitude, &new_phase, &new_amplitude);

  // Lock-free commit.
  uint64_t new_packed = pack_iq_state(new_phase, new_amplitude);
  atomic_store_explicit(&iq_state->packed_state, new_packed,
                        memory_order_relaxed);

  // Debug logging (rate limited)
  static double last_debug_log_time = 0.0;
  double current_optimization_time = atomic_load_explicit(
      &iq_state->last_optimization_time, memory_order_relaxed);
  if (current_optimization_time - last_debug_log_time >=
      CONSOLE_UPDATE_INTERVAL_SEC) {
    float phase_degrees = new_phase * (180.0f / (float)M_PI);
    float amplitude_percent = new_amplitude * 100.0f;
    float image_db =
        10.0f * log10f((float)iq_state->integrated_image_power + 1e-12f);

    log_debug(
        "IQ Correct: Phase: %+.3f deg | Amp: %+.3f %% | Image Pwr: %.1f dBFS",
        phase_degrees, amplitude_percent, image_db);
    last_debug_log_time = current_optimization_time;
  }
}

static void iq_correction_destroy(IqState *state) {
  if (state) {
    queue_destroy(&state->free_queue);
    queue_destroy(&state->data_queue);

    if (state->fft_plan)
      fft_destroy_plan(state->fft_plan);
    // Arena handles memory free
  }
}

bool iq_correction_run_initial_calibration(
    ModuleContext *context,
    size_t (*read_callback)(void *user_data, void *buffer, size_t bytes),
    void *user_data) {
  AppContext *app = context->app;

  if (!read_callback || app->module.input_bytes_per_iq_sample == 0) {
    log_warn("Cannot perform initial I/Q correction without valid input data "
             "or reader.");
    return true;
  }

  log_info("Performing initial I/Q calibration for file input...");

  s_calibrated_phase = 0.0f;
  s_calibrated_amplitude = 0.0f;

  size_t num_bytes =
      IQ_WORKING_BUFFER_LENGTH * app->module.input_bytes_per_iq_sample;
  void *raw_buffer =
      mem_arena_alloc(&app->process_chain.setup_arena, num_bytes, false);
  ComplexFloat *cf32_buffer = (ComplexFloat *)mem_arena_alloc(
      &app->process_chain.setup_arena,
      IQ_WORKING_BUFFER_LENGTH * sizeof(ComplexFloat), false);
  if (!raw_buffer || !cf32_buffer) {
    log_fatal("Failed to allocate temporary buffer for I/Q calibration.");
    return false;
  }

  SampleChunk temp_chunk;
  memset(&temp_chunk, 0, sizeof(SampleChunk));
  temp_chunk.raw_input_data = raw_buffer;
  temp_chunk.packet_sample_format = app->module.input_format;
  temp_chunk.current_buffer = cf32_buffer;

  void *dc_block_state = NULL;
  const DspModuleInterface *dc_block_api = get_dsp_module("dc_block");
  if (((AppConfig *)app->config)->dsp.dc_block.enable && dc_block_api) {
    double dummy_rate;
    dc_block_state = dc_block_api->initialize(
        context, app->module.input_info.sample_rate,
        app->module.input_info.sample_rate, &dummy_rate);
  }

  void *state = iq_correction_init((AppConfig *)context->config, app,
                                   &app->process_chain.setup_arena);

  for (int i = 0; i < 64; i++) {
    size_t bytes_read = read_callback(user_data, raw_buffer, num_bytes);
    if (bytes_read < num_bytes)
      break;

    temp_chunk.frames_read = IQ_WORKING_BUFFER_LENGTH;
    sample_convert_block_to_cf32(
        temp_chunk.raw_input_data, temp_chunk.current_buffer,
        temp_chunk.frames_read, temp_chunk.packet_sample_format,
        ((AppConfig *)app->config)->dsp.input_gain);

    if (dc_block_state) {
      temp_chunk.state = CHUNK_STATE_DATA;
      dc_block_api->process(dc_block_state, &temp_chunk);
    }

    iq_correction_run_estimation(state, temp_chunk.current_buffer);
    if (state) {
      atomic_store_explicit(&((IqState *)state)->last_optimization_time, 0.0,
                            memory_order_relaxed);
    }
  }

  if (state) {
    uint64_t final_packed = atomic_load_explicit(
        &((IqState *)state)->packed_state, memory_order_relaxed);
    unpack_iq_state(final_packed, &s_calibrated_phase, &s_calibrated_amplitude);
    iq_correction_destroy(state);
  }
  if (dc_block_state) {
    dc_block_api->cleanup(dc_block_state);
  }

  log_info("Initial I/Q calibration complete.");
  return true;
}

// ============================================================================
// == Internal Logic (Adapted from libairspyhf)
// ============================================================================

static void init_window(float *restrict window, int length) {
  const int length_minus_one = length - 1;
  for (int i = 0; i < length; i++) {
    window[i] =
        (float)(+0.35875f - 0.48829f * cos(2.0 * M_PI * i / length_minus_one) +
                0.14128f * cos(4.0 * M_PI * i / length_minus_one) -
                0.01168f * cos(6.0 * M_PI * i / length_minus_one));
  }
}

static void init_boost_window(void) {
  for (int k = 0; k < IQ_FFT_BINS; k++) {
    s_boost_window[k] =
        (float)(1.0 / IQ_BOOST_FACTOR +
                1.0 / exp(pow(k * 2.0 / IQ_BINS_TO_OPTIMIZE, 2.0)));
  }
}

static void apply_window(complex float *restrict buffer,
                         const float *restrict window, int length) {
  for (int i = 0; i < length; i++) {
    buffer[i] *= window[i];
  }
}

static float adjust_benchmark(complex float *iq, float phase, float amplitude,
                              int skip_power_calculation) {
  float sum = 0.0f;
  for (int i = 0; i < IQ_FFT_BINS; i++) {
    float real_part = crealf(iq[i]);
    float imag_part = cimagf(iq[i]);

    float new_real = real_part + phase * imag_part;
    float new_imag = imag_part + phase * real_part;

    new_real *= (1.0f + amplitude);
    new_imag *= (1.0f - amplitude);

    // Update buffer in-place for subsequent FFT
    iq[i] = new_real + I * new_imag;

    if (!skip_power_calculation) {
      sum += real_part * real_part + imag_part * imag_part;
    }
  }
  return sum;
}

static int
accumulate_fft_correlation(IqState *state, const complex float *restrict iq,
                           complex float *restrict correlation_accumulator,
                           int length, float base_phase, float base_amplitude,
                           int step) {
  complex float *fft_buffer = state->fft_buffer;
  int count = 0;
  float phase = base_phase + step * IQ_PHASE_STEP;
  float amplitude = base_amplitude + step * IQ_AMPLITUDE_STEP;

  for (int sample_offset = 0, window_index = 0;
       sample_offset <= length - IQ_FFT_BINS &&
       window_index < state->fft_integration;
       sample_offset += IQ_FFT_BINS / state->fft_overlap, window_index++) {
    memcpy(fft_buffer, iq + sample_offset, IQ_FFT_BINS * sizeof(complex float));
    float power = adjust_benchmark(fft_buffer, phase, amplitude, step);
    if (step == 0) {
      if (power > IQ_MINIMUM_POWER) {
        state->power_flags[window_index] = 1;
        state->integrated_total_power += power;
      } else {
        state->power_flags[window_index] = 0;
      }
    }
    if (state->power_flags[window_index] == 1) {
      count++;
      apply_window(fft_buffer, state->window_function, IQ_FFT_BINS);

      // Execute Liquid-DSP FFT (In-Place) and shift DC to IQ_FFT_BINS / 2
      fft_execute(state->fft_plan);
      fft_shift((liquid_float_complex *)fft_buffer, IQ_FFT_BINS);

      for (int bin = IQ_EDGE_BINS_TO_SKIP,
               mirror_bin = IQ_FFT_BINS - IQ_EDGE_BINS_TO_SKIP;
           bin <= IQ_FFT_BINS - IQ_EDGE_BINS_TO_SKIP; bin++, mirror_bin--) {
        correlation_accumulator[bin] +=
            fft_buffer[bin] * fft_buffer[mirror_bin];
        correlation_accumulator[mirror_bin] = correlation_accumulator[bin];
      }
      if (step == 0) {
        for (int bin = IQ_EDGE_BINS_TO_SKIP;
             bin <= IQ_FFT_BINS - IQ_EDGE_BINS_TO_SKIP; bin++) {
          float bin_power = crealf(fft_buffer[bin]) * crealf(fft_buffer[bin]) +
                            cimagf(fft_buffer[bin]) * cimagf(fft_buffer[bin]);
          state->boost_spectrum[bin] += bin_power;
          if (state->optimal_bin == IQ_FFT_BINS / 2) {
            state->integrated_image_power += bin_power;
          } else {
            state->integrated_image_power +=
                bin_power *
                s_boost_window[abs(IQ_FFT_BINS - bin - state->optimal_bin)];
          }
        }
      }
    }
  }

  return count;
}

static complex float
compute_correlation_utility(IqState *state,
                            const complex float *correlation_accumulator) {
  float inverse_edge_skip = 1.0f / IQ_EDGE_BINS_TO_SKIP;
  complex float weighted_sum = 0;

  for (int bin = IQ_EDGE_BINS_TO_SKIP,
           mirror_bin = IQ_FFT_BINS - IQ_EDGE_BINS_TO_SKIP;
       bin <= IQ_FFT_BINS - IQ_EDGE_BINS_TO_SKIP; bin++, mirror_bin--) {
    int distance = abs(bin - IQ_FFT_BINS / 2);
    if (distance > IQ_CENTER_BINS_TO_SKIP) {
      float weight = (distance > IQ_EDGE_BINS_TO_SKIP)
                         ? 1.0f
                         : (distance * inverse_edge_skip);
      if (state->optimal_bin != IQ_FFT_BINS / 2) {
        weight *= s_boost_window[abs(state->optimal_bin - bin)];
      }
      weight *= state->boost_spectrum[mirror_bin] /
                (state->boost_spectrum[bin] + IQ_EPSILON);
      weighted_sum += correlation_accumulator[bin] * weight;
    }
  }
  return weighted_sum;
}

static void estimate_imbalance(IqState *state, const complex float *restrict iq,
                               int length, float current_phase,
                               float current_amplitude, float *out_phase,
                               float *out_amplitude) {
  // Initialize outputs with current values in case we exit early
  *out_phase = current_phase;
  *out_amplitude = current_amplitude;

  if (state->reset_flag) {
    state->reset_flag = 0;
    state->integration_count = -IQ_BUFFERS_TO_SKIP_ON_RESET;
    state->maximum_image_power = 0;
  }

  if (state->integration_count < 0) {
    state->integration_count++;
    return;
  } else if (state->integration_count == 0) {
    state->integrated_image_power = 0;
    state->integrated_total_power = 0;
    memset(state->boost_spectrum, 0, IQ_FFT_BINS * sizeof(float));
    memset(state->correlation, 0, IQ_FFT_BINS * sizeof(complex float));
    memset(state->correlation_perturbed, 0,
           IQ_FFT_BINS * sizeof(complex float));
  }

  state->maximum_image_power *= IQ_MAX_POWER_DECAY;

  // 1. Compute Correlation (Current Parameters)
  int valid_windows =
      accumulate_fft_correlation(state, iq, state->correlation, length,
                                 current_phase, current_amplitude, 0);
  if (valid_windows == 0)
    return;
  state->integration_count += valid_windows;

  // 2. Compute Correlation (Perturbed Parameters)
  accumulate_fft_correlation(state, iq, state->correlation_perturbed, length,
                             current_phase, current_amplitude, 1);

  // Check integration limit
  if (state->integration_count <=
      state->correlation_integration * state->fft_integration)
    return;
  state->integration_count = 0;

  // Power Threshold Check
  if (state->optimal_bin == IQ_FFT_BINS / 2) {
    if (state->integrated_total_power < state->maximum_image_power)
      return;
    state->maximum_image_power = (float)state->integrated_total_power;
  } else {
    if (state->integrated_image_power -
            state->integrated_total_power * IQ_BOOST_WINDOW_NORMALIZATION <
        state->maximum_image_power * IQ_POWER_THRESHOLD)
      return;
    state->maximum_image_power =
        (float)(state->integrated_image_power -
                state->integrated_total_power * IQ_BOOST_WINDOW_NORMALIZATION);
  }

  // Calculate utility vectors
  complex float base_utility =
      compute_correlation_utility(state, state->correlation);
  complex float perturbed_utility =
      compute_correlation_utility(state, state->correlation_perturbed);

  // Update Phase
  float mu = cimagf(base_utility) - cimagf(perturbed_utility);
  if (fabs(mu) > IQ_MIN_DELTA_MU) {
    mu = cimagf(base_utility) / mu;
    if (mu < -IQ_MAX_MU)
      mu = -IQ_MAX_MU;
    else if (mu > IQ_MAX_MU)
      mu = IQ_MAX_MU;
  } else {
    mu = 0.0f;
  }
  float phase = current_phase + IQ_PHASE_STEP * mu;

  // Update Amplitude
  mu = crealf(base_utility) - crealf(perturbed_utility);
  if (fabs(mu) > IQ_MIN_DELTA_MU) {
    mu = crealf(base_utility) / mu;
    if (mu < -IQ_MAX_MU)
      mu = -IQ_MAX_MU;
    else if (mu > IQ_MAX_MU)
      mu = IQ_MAX_MU;
  } else {
    mu = 0.0f;
  }
  float amplitude = current_amplitude + IQ_AMPLITUDE_STEP * mu;

  // History Smoothing
  if (state->history_count < IQ_MAX_LOOKBACK)
    state->history_count++;

  state->raw_amplitudes[state->history_index] = amplitude;
  state->raw_phases[state->history_index] = phase;

  // Reset accumulators for moving average calculation
  phase = 0.0f;
  amplitude = 0.0f;

  for (int k = 0; k < state->history_count; k++) {
    int index =
        (state->history_index + IQ_MAX_LOOKBACK - k) & (IQ_MAX_LOOKBACK - 1);
    phase += state->raw_phases[index];
    amplitude += state->raw_amplitudes[index];
  }
  phase /= state->history_count;
  amplitude /= state->history_count;
  state->history_index = (state->history_index + 1) & (IQ_MAX_LOOKBACK - 1);

  // Commit new parameters to output pointers
  *out_phase = phase;
  *out_amplitude = amplitude;
}

// === DSP Module Interface Implementation ===

static void *iq_estimation_thread(void *arg) {
  IqState *state = (IqState *)arg;
  if (!state)
    return NULL;

  while (!is_shutdown_requested()) {
    void *buffer = queue_try_dequeue(&state->data_queue);
    if (buffer) {
      iq_correction_run_estimation(state, (ComplexFloat *)buffer);
      queue_enqueue(&state->free_queue, buffer);
    } else {
      platform_sleep(1);
    }
  }
  log_debug("I/Q optimization thread is exiting.");
  return NULL;
}

static bool dsp_iq_correction_start_background_threads(
    void *state, struct ThreadManager *thread_manager) {
  (void)state;
  return thread_manager_spawn(thread_manager, "I/Q Optimizer", PRIORITY_NORMAL,
                              iq_estimation_thread, state);
}

static void *dsp_iq_correction_init(ModuleContext *context, double input_rate,
                                    double target_output_rate,
                                    double *out_rate) {
  (void)target_output_rate;
  *out_rate = input_rate;
  AppConfig *config = (AppConfig *)context->config;
  if (!config->dsp.dc_block.enable) {
    log_error("--iq-correction requires --dc-block to be enabled.");
    return NULL;
  }
  log_info("I/Q Optimizer: Enabled (Automatic Image Rejection)");
  return iq_correction_init(config, context->app,
                            &context->app->process_chain.setup_arena);
}

static SampleChunk *dsp_iq_correction_process(void *state, SampleChunk *chunk) {
  IqState *iq_state = (IqState *)state;
  if (chunk->state == CHUNK_STATE_STREAM_RESET) {
    if (iq_state) {
      iq_state->current_estimation_collected = 0;
    }
  }

  // Copy uncorrected (DC-blocked) samples for asynchronous estimation first,
  // because estimate_imbalance() applies adjust_benchmark() internally.
  if (iq_state && chunk->frames_read > 0) {
    unsigned int frames_remaining = (unsigned int)chunk->frames_read;
    unsigned int read_offset = 0;

    while (frames_remaining > 0) {
      if (!iq_state->current_estimation_buffer) {
        iq_state->current_estimation_buffer =
            (ComplexFloat *)queue_try_dequeue(&iq_state->free_queue);
        if (!iq_state->current_estimation_buffer)
          break; // If no free buffers, skip estimation (drops frames)
        iq_state->current_estimation_collected = 0;
      }

      unsigned int to_copy =
          IQ_WORKING_BUFFER_LENGTH - iq_state->current_estimation_collected;
      if (to_copy > frames_remaining)
        to_copy = frames_remaining;

      memcpy(iq_state->current_estimation_buffer +
                 iq_state->current_estimation_collected,
             chunk->current_buffer + read_offset,
             to_copy * sizeof(ComplexFloat));

      iq_state->current_estimation_collected += to_copy;
      read_offset += to_copy;
      frames_remaining -= to_copy;

      if (iq_state->current_estimation_collected == IQ_WORKING_BUFFER_LENGTH) {
        if (!queue_enqueue(&iq_state->data_queue,
                           iq_state->current_estimation_buffer)) {
          queue_enqueue_forced(&iq_state->free_queue,
                               iq_state->current_estimation_buffer);
        }
        iq_state->current_estimation_buffer = NULL;
      }
    }
  }

  iq_correction_apply(iq_state, chunk->current_buffer, (int)chunk->frames_read);
  return chunk;
}

static void dsp_iq_correction_cleanup(void *state) {
  iq_correction_destroy((IqState *)state);
}

// No reset function needed for IQ Correction

static bool s_enable_iq_correction = false;

// clang-format off
static const struct argparse_option cli_options[] = {
        OPT_GROUP("I/Q Correction Options"),
        OPT_BOOLEAN(0, "iq-correction", &s_enable_iq_correction, "(Optional) Enable automatic I/Q imbalance correction.", NULL, 0, 0),
};
// clang-format on

static const struct argparse_option *
dsp_iq_correction_get_cli_options(int *count) {
  *count = sizeof(cli_options) / sizeof(cli_options[0]);
  return cli_options;
}

static bool dsp_iq_correction_validate_options(struct AppContext *app) {
  if (app && app->config) {
    AppConfig *config = (AppConfig *)app->config;
    config->dsp.iq_correction.enable = s_enable_iq_correction;
  }
  return true;
}

static bool dsp_iq_correction_is_active(AppContext *app,
                                        const char *stage_tag) {
  (void)stage_tag;
  return ((AppConfig *)app->config)->dsp.iq_correction.enable;
}

static const DspModuleInterface dsp_iq_correction_api = {
    .name = "iq_correction",
    .is_active = dsp_iq_correction_is_active,
    .start_background_threads = dsp_iq_correction_start_background_threads,
    .initialize = dsp_iq_correction_init,
    .process = dsp_iq_correction_process,
    .reset = NULL,
    .cleanup = dsp_iq_correction_cleanup,
    .validate_options = dsp_iq_correction_validate_options,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module module = {
      .name = "iq_correction",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_iq_correction_api,
      .get_cli_options = dsp_iq_correction_get_cli_options,
  };
  module_registry_add(&module);
}
