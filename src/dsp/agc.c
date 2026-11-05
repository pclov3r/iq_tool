/**
 * @file agc.c
 * @brief Implements the Output Automatic Gain Control module.
 *
 * This module provides a digital Automatic Gain Control (AGC) implementation
 * directly following the theory and architecture documented in:
 *
 *   Richard G. Lyons, "Understanding Digital Signal Processing", 3rd ed.,
 *   Section 13.30, Figure 13-77(a) (Logarithmic AGC circuit).
 *
 * Based on the foundational work of:
 *   Fred Harris & Gregory Smith, "On the Design, Implementation, and
 *   Performance of a Microprocessor-Controlled AGC System for a Digital
 *   Receiver", IEEE MILCOM, 1988.
 *
 * The deadband and block-level adaptation for software SDR were informed by
 * Chris Gianakopoulos's open-source AGC implementation for RTL-SDR:
 * https://github.com/wizardyesterday/AutomaticGainControl
 *
 * Lyons Fig. 13-77(a) Logarithmic AGC Block Diagram:
 *
 *   x(n) ───────>[ x ]───────────────────────────────────┬────────> y(n)
 *                 ▲                                      │
 *          Gain   │                                      ▼
 *             ┌───────┐                              ┌───────┐
 *             │Antilog│                              │   x   │ (squarer: |y(n)|²)
 *             └───────┘                              └───────┘
 *                 ▲                                      │
 *                 │                                      ▼
 *               ┌───┐                                ┌───────┐
 *          ┌───>│z⁻¹├───┐                            │  LPF  │ (mean power)
 *          │    └───┘   │                            └───────┘
 *          │            │                                │
 *          │            ▼                                ▼
 *          │          ┌───┐          ┌───┐           ┌───────┐
 *          └──────────┤ + │<─────────┤ x │<── (-) ───┤  Log  │
 *                     └───┘          └───┘           └───────┘
 *                                      ▲                 ▲ (+)
 *                                      │ α               │
 *                                                    2 log(R)
 *
 * In this software-defined radio implementation (with block adaptation and
 * deadband informed by Chris Gianakopoulos):
 *
 *   1. Power Measurement & LPF:
 *      The squarer + LPF + Log stages are computed across each block of N samples
 *      using double-precision RMS accumulation:
 *        P_dB = 10 * log10( (1/N) * sum(|x[i]|²) )
 *      Estimated output power: y_dB = P_dB + gain_db.
 *
 *   2. Error Detection & Deadband:
 *      error = R_target_db - y_dB
 *      If |error| <= deadband_db, error = 0 (signal passes through untouched).
 *
 *   3. Loop Integrator (LMS Update) & Gain Rails:
 *      gain_db(k+1) = clamp(gain_db(k) + alpha * error, gain_min_db, gain_max_db)
 *      target_gain_linear = 10^(gain_db / 20)
 *
 *   4. Continuous Slew Ramping (Antilog & Multiplier):
 *      To preserve Lyons' continuous-time feedback behavior and eliminate
 *      block-boundary step discontinuities, the linear gain is smoothly ramped
 *      sample-by-sample across the block from gain_linear_previous to target_gain_linear:
 *        samples[i] *= (gain_start + i * gain_step)
 */

#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "module.h"
#include "module_registry.h"
#include "process_chain_types.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// --- Default Configuration ---

/**
 * @def AGC_HARRIS_TARGET_DBFS
 * @brief Target RMS output level in dBFS for the Harris/LMS AGC.
 *
 * -18 dBFS gives high-PAPR signals with high peak-to-average power ratios
 * comfortable headroom below full scale. A signal arriving at -14 dBFS with a 1
 * dB deadband has an error of +4 dB which falls outside the deadband, so the
 * gain attenuates by 4 dB * alpha towards the target.
 */
#define AGC_HARRIS_TARGET_DBFS -18.0f

/**
 * @def AGC_HARRIS_DEADBAND_DB
 * @brief Deadband half-width in dB for the Harris/LMS AGC.
 *
 * If |error| <= deadband, gain is frozen and samples pass through with
 * the current gain unchanged. Signals within [TARGET-DEADBAND, TARGET+DEADBAND]
 * dBFS receive no gain adjustment at all.
 *
 * With the default target of -18 dBFS and deadband of 1 dB, signals
 * in the range [-19, -17] dBFS are passed through untouched. This covers
 * the natural volume variations of most signals without constant intervention.
 *
 * Harris's original paper used 1 dB for discrete hardware VGA steps.
 * While software AGC has infinite gain resolution, using a tight 1 dB
 * deadband ensures weak signals are properly amplified to the target, while
 * still saving CPU cycles when the signal is stable.
 */
#define AGC_HARRIS_DEADBAND_DB 1.0f

/**
 * @def AGC_HARRIS_ALPHA
 * @brief LMS loop filter coefficient for the Harris/LMS AGC (0 < alpha < 1).
 *
 * Controls convergence speed. Smaller = slower and more stable.
 *
 * This AGC operates once per block rather than once per sample, so it
 * ticks many times per second at typical block sizes. A much smaller
 * value than Harris's original 0.8 (for infrequently-called hardware
 * VGA) is therefore appropriate.
 *
 * At alpha 0.2 and a 20 dB error (very weak signal), gain moves 4 dB
 * per block — converging in roughly 5 blocks. For a 6 dB error (just
 * outside the deadband), gain moves 1.2 dB per block.
 */
#define AGC_HARRIS_ALPHA 0.2f

/**
 * @def AGC_HARRIS_GAIN_MIN_DB
 * @brief Minimum gain the Harris/LMS AGC may apply, in dB.
 *
 * Negative values allow attenuation of hot signals.
 * -20 dB allows meaningful attenuation without the loop going to extremes.
 */
#define AGC_HARRIS_GAIN_MIN_DB -20.0f

/**
 * @def AGC_HARRIS_GAIN_MAX_DB
 * @brief Maximum gain the Harris/LMS AGC may apply, in dB.
 *
 * +40 dB (100x linear) is enough to rescue a genuinely weak signal.
 * Caps gain to prevent amplifying a noise floor into something a decoder
 * mistakes for a signal.
 */
#define AGC_HARRIS_GAIN_MAX_DB 40.0f

// How often to emit periodic AGC runtime status at debug level (seconds).
// Applies to all profiles. 5 seconds is frequent enough to observe gain
// riding during a fade without flooding the log during normal operation.
#define AGC_LOG_INTERVAL_SEC 5.0f

/* =========================================================================
 * Harris/LMS AGC — internal state block
 * ======================================================================= */

typedef struct harris_agc_s {
  float gain_db;              /* Current gain in dB. Updated once per block.     */
  float gain_linear;          /* Cached linear scalar derived from gain_db.      */
  float gain_linear_previous; /* Linear scalar at start of block for slew ramp.  */
  float target_db;            /* Target RMS level in dBFS.                       */
  float deadband_db;          /* Deadband half-width in dB.                      */
  float alpha;                /* LMS loop filter coefficient (0 < alpha < 1).    */
  float gain_min_db;          /* Minimum permitted gain in dB.                   */
  float gain_max_db;          /* Maximum permitted gain in dB.                   */
  uint64_t samples_seen;      /* Total samples processed (for startup logging).  */

  // Extracted from DspContext
  bool enabled;
  double sample_rate_hz;
} HarrisAgc;

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/**
 * @brief Measures the RMS level of a block of complex samples in dBFS.
 *
 * Uses double precision to accumulate squared magnitudes, preventing
 * precision loss over large blocks of samples.
 */
static float measure_rms_dbfs(const ComplexFloat *samples,
                              unsigned int num_samples) {
  if (num_samples == 0) {
    return -120.0f;
  }

  double sum_squared = 0.0;
  for (unsigned int sample_index = 0; sample_index < num_samples;
       sample_index++) {
    double real_part = (double)crealf(samples[sample_index]);
    double imag_part = (double)cimagf(samples[sample_index]);
    sum_squared += (real_part * real_part + imag_part * imag_part);
  }

  double mean_square = sum_squared / (double)num_samples;
  if (mean_square < 1e-12) {
    return -120.0f;
  }

  return (float)(10.0 * log10(mean_square));
}

/**
 * @brief Runs one Harris/LMS AGC update for a block of samples.
 */
static void harris_agc_execute(HarrisAgc *agc, ComplexFloat *samples,
                               unsigned int num_samples) {
  /* Measure block RMS in dBFS arriving at the input (LPF level detector). */
  float rms_db = measure_rms_dbfs(samples, num_samples);

  /* y(n) = x(n) + g(n)  — estimated output level.
   * e(n) = R - y(n)     — deviation from target. */
  float output_db = rms_db + agc->gain_db;
  float error = agc->target_db - output_db;

  /* Deadband: freeze gain when signal is already close enough to target. */
  if (fabsf(error) <= agc->deadband_db) {
    error = 0.0f;
  }

  /* Gain rails: stop adjusting beyond the hard limits. */
  if (agc->gain_db >= agc->gain_max_db && error > 0.0f) {
    error = 0.0f;
  }
  if (agc->gain_db <= agc->gain_min_db && error < 0.0f) {
    error = 0.0f;
  }

  /* LMS update: g(n+1) = g(n) + alpha * e(n). */
  if (error != 0.0f) {
    agc->gain_db += agc->alpha * error;
    if (agc->gain_db > agc->gain_max_db) {
      agc->gain_db = agc->gain_max_db;
    }
    if (agc->gain_db < agc->gain_min_db) {
      agc->gain_db = agc->gain_min_db;
    }

    agc->gain_linear = powf(10.0f, agc->gain_db / 20.0f);
  }

  /* Apply smooth linear gain slew across the block from the previous gain
   * to the updated gain. This eliminates block-boundary step discontinuities,
   * preserving continuous feedback behavior per Lyons Fig. 13-77(a). */
  float gain_start = agc->gain_linear_previous;
  float gain_end = agc->gain_linear;
  float gain_step = (gain_end - gain_start) / (float)num_samples;
  float current_gain = gain_start;

  for (unsigned int sample_index = 0; sample_index < num_samples;
       sample_index++) {
    current_gain += gain_step;
    samples[sample_index] *= current_gain;
  }

  agc->gain_linear_previous = gain_end;
  agc->samples_seen += num_samples;
}

/* =========================================================================
 * Public API
 * ======================================================================= */

static HarrisAgc *agc_create(AppConfig *config, AppContext *app) {
  (void)config;
  if (!app->dsp.process_chain_agc.enable) {
    return NULL;
  }

  HarrisAgc *agc = (HarrisAgc *)mem_arena_alloc(&app->process_chain.setup_arena,
                                                sizeof(HarrisAgc), true);
  if (!agc) {
    log_fatal("Failed to allocate Harris AGC state.");
    return NULL;
  }

  agc->target_db = AGC_HARRIS_TARGET_DBFS;
  agc->deadband_db = AGC_HARRIS_DEADBAND_DB;
  agc->alpha = AGC_HARRIS_ALPHA;
  agc->gain_min_db = AGC_HARRIS_GAIN_MIN_DB;
  agc->gain_max_db = AGC_HARRIS_GAIN_MAX_DB;
  agc->gain_db = 0.0f; /* Start at unity gain. */
  agc->gain_linear = 1.0f;
  agc->gain_linear_previous = 1.0f;
  agc->samples_seen = 0;
  agc->enabled = app->dsp.process_chain_agc.enable;
  agc->sample_rate_hz = app->dsp.process_chain_sample_rate_hz;

  if (app->dsp.process_chain_agc.target_level > 0.0f) {
    agc->target_db =
        20.0f * log10f(app->dsp.process_chain_agc.target_level);
  }

  log_info("Enabled (Harris/LMS Block Tracker).");
  log_info("  Algorithm:  Harris/LMS, dB domain, block-level.");
  log_info("  Target:     %.1f dBFS", agc->target_db);
  log_info("  Deadband:   ±%.1f dB", agc->deadband_db);
  log_info("  Alpha:      %.2f", agc->alpha);
  log_info("  Gain range: [%.1f dB .. %.1f dB]", agc->gain_min_db,
           agc->gain_max_db);

  return agc;
}

static void agc_apply(HarrisAgc *agc, ComplexFloat *samples,
                      unsigned int num_samples) {
  if (!agc || !agc->enabled || num_samples == 0)
    return;

  /* Capture pre-execute gain for deadband detection in the log. */
  float gain_db_before = agc->gain_db;

  /* Harris/LMS update and slew-ramped gain application. */
  harris_agc_execute(agc, samples, num_samples);

  /* Log on first block. */
  if (agc->samples_seen == num_samples) {
    log_info("First block - gain: %.2f dB (%.4fx).", agc->gain_db,
             agc->gain_linear);
  }

  /* Periodic status. */
  uint64_t previous_samples_seen = agc->samples_seen - num_samples;
  uint64_t period = (uint64_t)(agc->sample_rate_hz * AGC_LOG_INTERVAL_SEC);

  if (period > 0 && (previous_samples_seen / period) != (agc->samples_seen / period)) {
    bool in_deadband = (fabsf(agc->gain_db - gain_db_before) < 1e-6f);
    log_debug("Gain=%.2f dB  %s", agc->gain_db,
              in_deadband ? "(deadband — passing through unchanged)"
                          : "(adjusting)");
  }
}

static void agc_reset(HarrisAgc *agc) {
  if (agc) {
    agc->gain_db = 0.0f;
    agc->gain_linear = 1.0f;
    agc->gain_linear_previous = 1.0f;
    agc->samples_seen = 0;
  }
}

static void agc_destroy(HarrisAgc *agc) {
  if (agc) {
    // free(agc); // Memory arena handles this
  }
}

// === DSP Module Interface Implementation ===

static void *dsp_agc_init(ModuleContext *ctx, double input_rate,
                          double target_output_rate, double *out_rate) {
  (void)target_output_rate;
  *out_rate = input_rate;
  return agc_create((AppConfig *)ctx->config, ctx->app);
}

static SampleChunk *dsp_agc_process(void *state, SampleChunk *chunk) {
  HarrisAgc *agc = (HarrisAgc *)state;
  if (chunk->state == CHUNK_STATE_STREAM_RESET) {
    agc_reset(agc);
  }
  agc_apply(agc, chunk->current_buffer, chunk->frames_to_write);
  return chunk;
}

static void dsp_agc_cleanup(void *state) { agc_destroy((HarrisAgc *)state); }

static void dsp_agc_reset_api(void *state) { agc_reset((HarrisAgc *)state); }

static bool s_baseband_agc = false;
static float s_baseband_agc_target = 0.0f;
static bool s_output_agc = false;
static float s_output_agc_target = 0.0f;

// clang-format off
static const struct argparse_option cli_options[] = {
        OPT_GROUP("AGC Options"),
        OPT_BOOLEAN(0, "baseband-agc", &s_baseband_agc, "Enable automatic gain control on the baseband signal before " "demodulation.", NULL, 0, 0),
        OPT_FLOAT(0, "baseband-agc-target", &s_baseband_agc_target, "AGC target magnitude (0.0 - 1.0). (Default: 0.12)", NULL, 0, 0),
        OPT_BOOLEAN( 0, "output-agc", &s_output_agc, "Enable automatic gain control on the output signal before saving.", NULL, 0, 0),
        OPT_FLOAT(0, "output-agc-target", &s_output_agc_target, "AGC target magnitude (0.0 - 1.0). (Default: 0.12)", NULL, 0, 0),
};
// clang-format on

static const struct argparse_option *dsp_agc_get_cli_options(int *count) {
  *count = sizeof(cli_options) / sizeof(cli_options[0]);
  return cli_options;
}

static bool dsp_agc_validate_options(struct AppContext *app) {
  if (app && app->config) {
    AppConfig *config = (AppConfig *)app->config;
    if (s_baseband_agc) {
      config->dsp.baseband_agc.enable = true;
    }
    if (s_baseband_agc_target != 0.0f) {
      config->dsp.baseband_agc.target_level = s_baseband_agc_target;
    }
    if (s_output_agc) {
      config->dsp.output_agc.enable = true;
    }
    if (s_output_agc_target != 0.0f) {
      config->dsp.output_agc.target_level = s_output_agc_target;
    }
  }
  return true;
}

static bool dsp_agc_is_active(AppContext *app, const char *stage_tag) {
  (void)stage_tag;
  return app->dsp.process_chain_agc.enable;
}

static const DspModuleInterface dsp_agc_api = {
    .name = "agc",
    .is_active = dsp_agc_is_active,
    .initialize = dsp_agc_init,
    .process = dsp_agc_process,
    .reset = dsp_agc_reset_api,
    .cleanup = dsp_agc_cleanup,
    .validate_options = dsp_agc_validate_options,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module agc_module = {
      .name = "agc",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_agc_api,
      .get_cli_options = dsp_agc_get_cli_options,
  };
  module_registry_add(&agc_module);
}
