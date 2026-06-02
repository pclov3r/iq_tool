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

#include "agc.h"
#include "constants.h"
#include "log.h"
#include "utilities.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

/* =========================================================================
 * Harris/LMS AGC — internal state block
 * ======================================================================= */

typedef struct harris_agc_s {
    float    gain_db;      /* Current gain in dB. Updated once per block.    */
    float    gain_linear;  /* Cached linear scalar derived from gain_db.     */
    float    target_db;    /* Target RMS level in dBFS.                      */
    float    deadband_db;  /* Deadband half-width in dB.                     */
    float    alpha;        /* LMS loop filter coefficient (0 < alpha < 1).   */
    float    gain_min_db;  /* Minimum permitted gain in dB.                  */
    float    gain_max_db;  /* Maximum permitted gain in dB.                  */
    uint64_t samples_seen; /* Total samples processed (for startup logging). */
} HarrisAgc;

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/**
 * @brief Blanks (zeros) any sample whose magnitude exceeds the threshold.
 */
static void apply_impulse_blanker(ComplexFloat *samples,
                                   unsigned int  num_samples,
                                   float         threshold)
{
    if (threshold < 1e-9f) return;

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
                                unsigned int        num_samples)
{
    if (num_samples == 0) return -120.0f;

    float sum_sq = 0.0f;
    for (unsigned int i = 0; i < num_samples; i++) {
        float r = crealf(samples[i]);
        float j = cimagf(samples[i]);
        sum_sq += (r * r + j * j);
    }

    float rms = sqrtf(sum_sq / (float)num_samples);
    if (rms < 1e-9f) return -120.0f;

    return 20.0f * log10f(rms);
}

/**
 * @brief Runs one Harris/LMS AGC update for a block of samples.
 */
static void harris_agc_execute(HarrisAgc    *h,
                                ComplexFloat *samples,
                                unsigned int  num_samples)
{
    /* Measure block RMS in dBFS arriving at the input. */
    float rms_db    = measure_rms_dbfs(samples, num_samples);

    /* y(n) = x(n) + g(n)  — estimated output level.
     * e(n) = R - y(n)     — deviation from target. */
    float output_db = rms_db + h->gain_db;
    float error     = h->target_db - output_db;

    /* Deadband: freeze gain when signal is already close enough to target. */
    if (fabsf(error) <= h->deadband_db) {
        error = 0.0f;
    }

    /* Gain rails: stop adjusting beyond the hard limits. */
    if (h->gain_db >= h->gain_max_db && error > 0.0f) error = 0.0f;
    if (h->gain_db <= h->gain_min_db && error < 0.0f) error = 0.0f;

    /* LMS update: g(n+1) = g(n) + alpha * e(n). */
    if (error != 0.0f) {
        h->gain_db += h->alpha * error;

        if (h->gain_db > h->gain_max_db) h->gain_db = h->gain_max_db;
        if (h->gain_db < h->gain_min_db) h->gain_db = h->gain_min_db;

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

bool agc_create(AppConfig *config, AppContext *app)
{
    if (!config->dsp.agc.enable) {
        app->dsp.agc.harris_object = NULL;
        return true;
    }

    /* Initialise common runtime state. */
    app->dsp.agc.current_gain   = 1.0f;
    app->dsp.agc.samples_seen   = 0;

    HarrisAgc *h = (HarrisAgc *)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(HarrisAgc), true);
    if (!h) {
        log_fatal("AGC: Failed to allocate Harris AGC state.");
        return false;
    }

    h->target_db    = AGC_HARRIS_TARGET_DBFS;
    h->deadband_db  = AGC_HARRIS_DEADBAND_DB;
    h->alpha        = AGC_HARRIS_ALPHA;
    h->gain_min_db  = AGC_HARRIS_GAIN_MIN_DB;
    h->gain_max_db  = AGC_HARRIS_GAIN_MAX_DB;
    h->gain_db      = 0.0f;  /* Start at unity gain. */
    h->gain_linear  = 1.0f;
    h->samples_seen = 0;

    if (config->dsp.agc.target_level_arg > 0.0f) {
        h->target_db = 20.0f * log10f(config->dsp.agc.target_level_arg);
    }

    app->dsp.agc.harris_object = (struct harris_agc_s *)h;

    log_info("AGC: Enabled (Harris/LMS Block Tracker).");
    log_info("AGC:   Algorithm:  Harris/LMS, dB domain, block-level.");
    log_info("AGC:   Target:     %.1f dBFS", h->target_db);
    log_info("AGC:   Deadband:   ±%.1f dB", h->deadband_db);
    log_info("AGC:   Alpha:      %.2f", h->alpha);
    log_info("AGC:   Gain range: [%.1f dB .. %.1f dB]", h->gain_min_db, h->gain_max_db);
    log_info("AGC:   Blanker:    threshold magnitude > %.1f", (double)AGC_BLANKER_THRESHOLD);

    return true;
}

void agc_apply(DspContext *dsp, ComplexFloat *samples, unsigned int num_samples)
{
    if (!dsp->config->dsp.agc.enable || num_samples == 0) return;

    HarrisAgc *h = (HarrisAgc *)dsp->agc.harris_object;
    if (!h) return;

    /* Stage 1: Impulse blanker. */
    apply_impulse_blanker(samples, num_samples, AGC_BLANKER_THRESHOLD);

    /* Capture pre-execute gain for deadband detection in the log. */
    float gain_db_before = h->gain_db;

    /* Stage 2: Harris/LMS update. */
    harris_agc_execute(h, samples, num_samples);

    /* Log on first block. */
    if (h->samples_seen == num_samples) {
        log_info("AGC: First block - gain: %.2f dB (%.4fx).",
                 h->gain_db, h->gain_linear);
    }

    /* Periodic status. */
    uint64_t prev = dsp->agc.samples_seen;
    dsp->agc.samples_seen += num_samples;
    uint64_t period = (uint64_t)(dsp->config->output_sample_rate.rate_hz * AGC_LOG_INTERVAL_SEC);

    if (period > 0 && (prev / period) != (dsp->agc.samples_seen / period)) {
        bool in_deadband = (fabsf(h->gain_db - gain_db_before) < 1e-6f);
        log_debug("AGC: gain=%.2f dB  %s",
                  h->gain_db,
                  in_deadband ? "(deadband — passing through unchanged)"
                              : "(adjusting)");
    }

    dsp->agc.current_gain = h->gain_linear;
}

void agc_reset(DspContext *dsp)
{
    HarrisAgc *h = (HarrisAgc *)dsp->agc.harris_object;
    if (h) {
        h->gain_db      = 0.0f;
        h->gain_linear  = 1.0f;
        h->samples_seen = 0;
    }
    dsp->agc.current_gain  = 1.0f;
    dsp->agc.samples_seen  = 0;
}

void agc_destroy(AppContext *app)
{
    app->dsp.agc.harris_object = NULL;
}

int agc_populate_cli_options(struct argparse_option *buffer, struct AppConfig *config)
{
    struct argparse_option options[] = {
        OPT_GROUP("Output Automatic Gain Control"),
        OPT_BOOLEAN(0, "output-agc", &config->dsp.agc.enable,
                    "Enable automatic gain control on the output.", NULL, 0, 0),
        OPT_FLOAT(0, "agc-target", &config->dsp.agc.target_level_arg,
                    "AGC target magnitude (0.0 - 1.0). (Default: 0.12)", NULL, 0, 0),
    };

    size_t count = sizeof(options) / sizeof(options[0]);
    memcpy(buffer, options, sizeof(options));
    return (int)count;
}

bool agc_validate_options(AppConfig *config) {
    if (config->dsp.agc.enable) {
        if (config->dsp.agc.target_level_arg != 0.0f) {
            if (config->dsp.agc.target_level_arg <= 0.0f || config->dsp.agc.target_level_arg > 1.0f) {
                log_error("Invalid AGC target level %.2f. Must be between 0.0 and 1.0.", config->dsp.agc.target_level_arg);
                return false;
            }
            config->dsp.agc.target_level = config->dsp.agc.target_level_arg;
        }

        if (config->dsp.raw_passthrough) {
            log_error("Option --output-agc cannot be used with --raw-passthrough.");
            return false;
        }

        if (config->dsp.output_gain != 1.0f) {
            log_error("Conflicting options: --output-agc and --output-gain-multiplier cannot be used together.");
            return false;
        }

        if (config->dsp.input_gain_provided && config->dsp.input_gain != 1.0f) {
            log_warn("Both --input-gain-multiplier and --output-agc are set.");
            log_warn("Manual gain is applied at input, but AGC will override the final volume at output.");
        }
    }
    return true;
}
