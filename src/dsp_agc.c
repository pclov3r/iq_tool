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

#include "dsp_agc.h"
#include "agc.h"
#include "constants.h"
#include "log.h"
#include "module.h"
#include "process_chain_types.h"
#include <math.h>
#include <stdio.h>

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
static void harris_agc_execute(HarrisAgc *h, ComplexFloat *samples,
                               unsigned int num_samples) {
  /* Measure block RMS in dBFS arriving at the input. */
  float rms_db = measure_rms_dbfs(samples, num_samples);

  /* y(n) = x(n) + g(n)  — estimated output level.
   * e(n) = R - y(n)     — deviation from target. */
  float output_db = rms_db + h->gain_db;
  float error = h->target_db - output_db;

  /* Deadband: freeze gain when signal is already close enough to target. */
  if (fabsf(error) <= h->deadband_db) {
    error = 0.0f;
  }

  /* Gain rails: stop adjusting beyond the hard limits. */
  if (h->gain_db >= h->gain_max_db && error > 0.0f)
    error = 0.0f;
  if (h->gain_db <= h->gain_min_db && error < 0.0f)
    error = 0.0f;

  /* LMS update: g(n+1) = g(n) + alpha * e(n). */
  if (error != 0.0f) {
    h->gain_db += h->alpha * error;

    if (h->gain_db > h->gain_max_db)
      h->gain_db = h->gain_max_db;
    if (h->gain_db < h->gain_min_db)
      h->gain_db = h->gain_min_db;

    h->gain_linear = powf(10.0f, h->gain_db / 20.0f);
  }

  /* Apply linear gain to all samples in-place. */
  for (unsigned int i = 0; i < num_samples; i++) {
    samples[i] *= h->gain_linear;
  }

  h->samples_seen += num_samples;
}

/* =========================================================================
 * Public API
 * ======================================================================= */

bool agc_create(AppConfig *config, AppContext *app) {
  (void)config;
  if (!app->dsp.process_chain_agc.enable) {
    app->dsp.agc.harris_object = NULL;
    return true;
  }

  /* Initialise common runtime state. */
  app->dsp.agc.current_gain = 1.0f;
  app->dsp.agc.samples_seen = 0;

  HarrisAgc *h = (HarrisAgc *)mem_arena_alloc(&app->process_chain.setup_arena,
                                              sizeof(HarrisAgc), true);
  if (!h) {
    log_fatal("AGC: Failed to allocate Harris AGC state.");
    return false;
  }

  h->target_db = AGC_HARRIS_TARGET_DBFS;
  h->deadband_db = AGC_HARRIS_DEADBAND_DB;
  h->alpha = AGC_HARRIS_ALPHA;
  h->gain_min_db = AGC_HARRIS_GAIN_MIN_DB;
  h->gain_max_db = AGC_HARRIS_GAIN_MAX_DB;
  h->gain_db = 0.0f; /* Start at unity gain. */
  h->gain_linear = 1.0f;
  h->samples_seen = 0;

  if (app->dsp.process_chain_agc.target_level_arg > 0.0f) {
    h->target_db = 20.0f * log10f(app->dsp.process_chain_agc.target_level_arg);
  }

  app->dsp.agc.harris_object = (struct harris_agc_s *)h;

  log_info("AGC: Enabled (Harris/LMS Block Tracker).");
  log_info("AGC:   Algorithm:  Harris/LMS, dB domain, block-level.");
  log_info("AGC:   Target:     %.1f dBFS", h->target_db);
  log_info("AGC:   Deadband:   ±%.1f dB", h->deadband_db);
  log_info("AGC:   Alpha:      %.2f", h->alpha);
  log_info("AGC:   Gain range: [%.1f dB .. %.1f dB]", h->gain_min_db,
           h->gain_max_db);
  log_info("AGC:   Blanker:    threshold magnitude > %.1f",
           (double)AGC_BLANKER_THRESHOLD);

  return true;
}

void agc_apply(DspContext *dsp, ComplexFloat *samples,
               unsigned int num_samples) {
  if (!dsp->process_chain_agc.enable || num_samples == 0)
    return;

  HarrisAgc *h = (HarrisAgc *)dsp->agc.harris_object;
  if (!h)
    return;

  /* Stage 1: Impulse blanker. */
  apply_impulse_blanker(samples, num_samples, AGC_BLANKER_THRESHOLD);

  /* Capture pre-execute gain for deadband detection in the log. */
  float gain_db_before = h->gain_db;

  /* Stage 2: Harris/LMS update. */
  harris_agc_execute(h, samples, num_samples);

  /* Log on first block. */
  if (h->samples_seen == num_samples) {
    log_info("AGC: First block - gain: %.2f dB (%.4fx).", h->gain_db,
             h->gain_linear);
  }

  /* Periodic status. */
  uint64_t prev = dsp->agc.samples_seen;
  dsp->agc.samples_seen += num_samples;
  uint64_t period =
      (uint64_t)(dsp->process_chain_sample_rate_hz * AGC_LOG_INTERVAL_SEC);

  if (period > 0 && (prev / period) != (dsp->agc.samples_seen / period)) {
    bool in_deadband = (fabsf(h->gain_db - gain_db_before) < 1e-6f);
    log_debug("AGC: gain=%.2f dB  %s", h->gain_db,
              in_deadband ? "(deadband — passing through unchanged)"
                          : "(adjusting)");
  }

  dsp->agc.current_gain = h->gain_linear;
}

void agc_reset(DspContext *dsp) {
  HarrisAgc *h = (HarrisAgc *)dsp->agc.harris_object;
  if (h) {
    h->gain_db = 0.0f;
    h->gain_linear = 1.0f;
    h->samples_seen = 0;
  }
  dsp->agc.current_gain = 1.0f;
  dsp->agc.samples_seen = 0;
}

void agc_destroy(AppContext *app) { app->dsp.agc.harris_object = NULL; }

// === DSP Module Interface Implementation ===

static bool dsp_agc_init(ModuleContext *ctx) {
  return agc_create((AppConfig *)ctx->config, ctx->app);
}

static SampleChunk *dsp_agc_process(ModuleContext *ctx, SampleChunk *chunk) {
  if (chunk->stream_discontinuity_event) {
    agc_reset(&ctx->app->dsp);
  }
  agc_apply(&ctx->app->dsp, chunk->post_resample_buffer,
            chunk->frames_to_write);
  return chunk;
}

static void dsp_agc_cleanup(ModuleContext *ctx) { agc_destroy(ctx->app); }

static void dsp_agc_reset_api(ModuleContext *ctx) {
  if (ctx && ctx->app) {
    agc_reset(&ctx->app->dsp);
  }
}

static bool s_baseband_agc = false;
static float s_baseband_agc_target = 0.0f;
static bool s_output_agc = false;
static float s_output_agc_target = 0.0f;

static const struct argparse_option cli_options[] = {
    OPT_GROUP("AGC Options"),
    OPT_BOOLEAN(0, "baseband-agc", &s_baseband_agc,
                "Enable automatic gain control on the baseband signal before "
                "demodulation.",
                NULL, 0, 0),
    OPT_FLOAT(0, "baseband-agc-target", &s_baseband_agc_target,
              "AGC target magnitude (0.0 - 1.0). (Default: 0.12)", NULL, 0, 0),
    OPT_BOOLEAN(
        0, "output-agc", &s_output_agc,
        "Enable automatic gain control on the output signal before saving.",
        NULL, 0, 0),
    OPT_FLOAT(0, "output-agc-target", &s_output_agc_target,
              "AGC target magnitude (0.0 - 1.0). (Default: 0.12)", NULL, 0, 0),
};

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

const struct DspModuleInterface *dsp_agc_get_api(void) { return &dsp_agc_api; }
