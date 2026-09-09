/**
 * @file agc.c
 * @brief Implements the Output Automatic Gain Control module.
 *
 * This module provides a self-contained Harris/LMS implementation inspired by
 * the algorithm described in:
 *
 *   Fred Harris & Gregory Smith, "On the Design, Implementation, and
 *   Performance of a Microprocessor-Controlled AGC System for a Digital
 *   Receiver", and documented in Richard G. Lyons, "Understanding Digital
 *   Signal Processing", 3rd ed., Section 13.30.
 *
 * The deadband and block-level adaptation for software SDR were informed by
 * Chris Gianakopoulos's open-source AGC implementation for RTL-SDR:
 * https://github.com/wizardyesterday/AutomaticGainControl
 *
 * The Harris algorithm works in the dB domain, treating the AGC as a
 * linear system:
 *
 *   y(n)   = x(n) + g(n)          [signal + gain, both in dB]
 *   e(n)   = R - y(n)             [error from target R]
 *   g(n+1) = g(n) + alpha * e(n)  [LMS gain update]
 *
 * A deadband prevents any gain adjustment when the signal is already
 * within a target range. This ensures that signals already in a good
 * range are passed through untouched — if the signal is already
 * close to the target, gain stays at whatever it currently is (typically
 * 0 dB) and samples are unchanged. No soft limiter is used — the output
 * is always a clean linear multiply.
 *
 * Signal chain (per block):
 *
 *   input samples
 *       │
 *       ▼
 *   [1] Impulse Blanker   – zeros samples whose magnitude exceeds
 *                           AGC_BLANKER_THRESHOLD before the
 *                           RMS measurement, protecting the level
 *                           estimator from impulse noise spikes.
 *       │
 *       ▼
 *   [2] Harris/LMS AGC    – measures block RMS in dBFS, computes error
 *                           from target, applies deadband and gain rails,
 *                           updates gain via LMS, applies linear scalar.
 *       │
 *       ▼
 *   output samples
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

// Harris/LMS AGC Logic
//
// The algorithm operates entirely in dB (treating the AGC as a linear
// system), applies a deadband so signals already in a good range are
// passed through untouched, and applies only a clean linear gain scalar
// to the samples — no nonlinear soft limiting.
//
// Signal chain:
//   [1] Pre-AGC impulse blanker  — zeros samples whose magnitude exceeds
//                                   AGC_BLANKER_THRESHOLD.
//   [2] Harris/LMS gain loop     — block RMS measurement, dB-domain LMS
//                                   update with deadband and gain rails.
//                                   Output is always a linear multiply.

/**
 * @def AGC_HARRIS_TARGET_DBFS
 * @brief Target RMS output level in dBFS for the Harris/LMS AGC.
 *
 * -18 dBFS gives high-PAPR signals with high peak-to-average power ratios
 * comfortable headroom below full scale. A signal arriving at -14 dBFS with a 6
 * dB deadband has an error of +4 dB which falls inside the deadband, so the
 * gain stays at 0 dB and the signal passes through untouched.
 *
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

// Impulse blanker threshold (absolute IQ magnitude).
// Any sample whose magnitude exceeds this value is zeroed before entering
// the AGC. Legitimate normalised signals should never exceed 1.0; hardware
// impulse artefacts commonly exceed 5-10. A value of 2.0 gives comfortable
// margin between the two without risk of blanking valid signal content.
#define AGC_BLANKER_THRESHOLD 2.0f

// How often to emit periodic AGC runtime status at debug level (seconds).
// Applies to all profiles. 5 seconds is frequent enough to observe gain
// riding during a fade without flooding the log during normal operation.
#define AGC_LOG_INTERVAL_SEC 5.0f

/* =========================================================================
 * Harris/LMS AGC — internal state block
 * ======================================================================= */

typedef struct harris_agc_s {
  float gain_db;         /* Current gain in dB. Updated once per block.    */
  float gain_linear;     /* Cached linear scalar derived from gain_db.     */
  float target_db;       /* Target RMS level in dBFS.                      */
  float deadband_db;     /* Deadband half-width in dB.                     */
  float alpha;           /* LMS loop filter coefficient (0 < alpha < 1).   */
  float gain_min_db;     /* Minimum permitted gain in dB.                  */
  float gain_max_db;     /* Maximum permitted gain in dB.                  */
  uint64_t samples_seen; /* Total samples processed (for startup logging). */

  // Extracted from DspContext
  bool enabled;
  double sample_rate_hz;
} HarrisAgc;

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/**
 * @brief Blanks (zeros) any sample whose magnitude exceeds the threshold.
 */
static void apply_impulse_blanker(ComplexFloat *samples,
                                  unsigned int num_samples, float threshold) {
  if (threshold < 1e-9f)
    return;

  float threshold_sq = threshold * threshold;

  for (unsigned int i = 0; i < num_samples; i++) {
    float r = crealf(samples[i]);
    float j = cimagf(samples[i]);

    if ((r * r + j * j) > threshold_sq) {
      samples[i] = 0.0f + 0.0f * I;
    }
  }
}

/**
 * @brief Measures the RMS level of a block of complex samples in dBFS.
 */
static float measure_rms_dbfs(const ComplexFloat *samples,
                              unsigned int num_samples) {
  if (num_samples == 0)
    return -120.0f;

  float sum_sq = 0.0f;
  for (unsigned int i = 0; i < num_samples; i++) {
    float r = crealf(samples[i]);
    float j = cimagf(samples[i]);
    sum_sq += (r * r + j * j);
  }

  float rms = sqrtf(sum_sq / (float)num_samples);
  if (rms < 1e-9f)
    return -120.0f;

  return 20.0f * log10f(rms);
}

/**
 * @brief Runs one Harris/LMS AGC update for a block of samples.
 */
static void harris_agc_execute(HarrisAgc *agc, ComplexFloat *samples,
                               unsigned int num_samples) {
  /* Measure block RMS in dBFS arriving at the input. */
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
  if (agc->gain_db >= agc->gain_max_db && error > 0.0f)
    error = 0.0f;
  if (agc->gain_db <= agc->gain_min_db && error < 0.0f)
    error = 0.0f;

  /* LMS update: g(n+1) = g(n) + alpha * e(n). */
  if (error != 0.0f) {
    agc->gain_db += agc->alpha * error;

    if (agc->gain_db > agc->gain_max_db)
      agc->gain_db = agc->gain_max_db;
    if (agc->gain_db < agc->gain_min_db)
      agc->gain_db = agc->gain_min_db;

    agc->gain_linear = powf(10.0f, agc->gain_db / 20.0f);
  }

  /* Apply linear gain to all samples in-place. */
  for (unsigned int i = 0; i < num_samples; i++) {
    samples[i] *= agc->gain_linear;
  }

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
    log_fatal("AGC: Failed to allocate Harris AGC state.");
    return NULL;
  }

  agc->target_db = AGC_HARRIS_TARGET_DBFS;
  agc->deadband_db = AGC_HARRIS_DEADBAND_DB;
  agc->alpha = AGC_HARRIS_ALPHA;
  agc->gain_min_db = AGC_HARRIS_GAIN_MIN_DB;
  agc->gain_max_db = AGC_HARRIS_GAIN_MAX_DB;
  agc->gain_db = 0.0f; /* Start at unity gain. */
  agc->gain_linear = 1.0f;
  agc->samples_seen = 0;
  agc->enabled = app->dsp.process_chain_agc.enable;
  agc->sample_rate_hz = app->dsp.process_chain_sample_rate_hz;

  if (app->dsp.process_chain_agc.target_level_arg > 0.0f) {
    agc->target_db =
        20.0f * log10f(app->dsp.process_chain_agc.target_level_arg);
  }

  log_info("AGC: Enabled (Harris/LMS Block Tracker).");
  log_info("AGC:   Algorithm:  Harris/LMS, dB domain, block-level.");
  log_info("AGC:   Target:     %.1f dBFS", agc->target_db);
  log_info("AGC:   Deadband:   ±%.1f dB", agc->deadband_db);
  log_info("AGC:   Alpha:      %.2f", agc->alpha);
  log_info("AGC:   Gain range: [%.1f dB .. %.1f dB]", agc->gain_min_db,
           agc->gain_max_db);
  log_info("AGC:   Blanker:    threshold magnitude > %.1f",
           (double)AGC_BLANKER_THRESHOLD);

  return agc;
}

static void agc_apply(HarrisAgc *agc, ComplexFloat *samples,
                      unsigned int num_samples) {
  if (!agc || !agc->enabled || num_samples == 0)
    return;

  /* Stage 1: Impulse blanker. */
  apply_impulse_blanker(samples, num_samples, AGC_BLANKER_THRESHOLD);

  /* Capture pre-execute gain for deadband detection in the log. */
  float gain_db_before = agc->gain_db;

  /* Stage 2: Harris/LMS update. */
  harris_agc_execute(agc, samples, num_samples);

  /* Log on first block. */
  if (agc->samples_seen == num_samples) {
    log_info("AGC: First block - gain: %.2f dB (%.4fx).", agc->gain_db,
             agc->gain_linear);
  }

  /* Periodic status. */
  uint64_t prev = agc->samples_seen - num_samples;
  uint64_t period = (uint64_t)(agc->sample_rate_hz * AGC_LOG_INTERVAL_SEC);

  if (period > 0 && (prev / period) != (agc->samples_seen / period)) {
    bool in_deadband = (fabsf(agc->gain_db - gain_db_before) < 1e-6f);
    log_debug("AGC: gain=%.2f dB  %s", agc->gain_db,
              in_deadband ? "(deadband — passing through unchanged)"
                          : "(adjusting)");
  }
}

static void agc_reset(HarrisAgc *agc) {
  if (agc) {
    agc->gain_db = 0.0f;
    agc->gain_linear = 1.0f;
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
  if (chunk->stream_discontinuity_event) {
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
    ((AppConfig *)app->config)->dsp.baseband_agc.enable = s_baseband_agc;
    ((AppConfig *)app->config)->dsp.baseband_agc.target_level_arg =
        s_baseband_agc_target;
    ((AppConfig *)app->config)->dsp.output_agc.enable = s_output_agc;
    ((AppConfig *)app->config)->dsp.output_agc.target_level_arg =
        s_output_agc_target;
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
    .get_cli_options = dsp_agc_get_cli_options,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "agc",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_agc_api,
  };
  module_registry_add(&m);
}
