/**
 * @file agc.c
 * @brief Implements the Output Automatic Gain Control module.
 *
 * Three profiles are supported:
 *
 *  DX      - Slow liquid-dsp RMS tracking for weak/fading analog signals.
 *  LOCAL   - Fast liquid-dsp RMS tracking for strong analog signals.
 *  DIGITAL - Harris/LMS block-level AGC for digital signals (OFDM, etc.).
 *            Operates entirely in the dB domain with a deadband so that
 *            signals already in a good range are passed through untouched.
 *            No soft limiter — the output is always a clean linear multiply.
 *
 * DX and LOCAL use agc_crcf from liquid-dsp as their core gain tracking
 * loop. These profiles feed analog decoders where gain ripple is benign.
 *
 * DIGITAL uses a self-contained Harris/LMS implementation inspired by
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
 * within +/- AGC_DIGITAL_HARRIS_DEADBAND_DB of the target. This is the
 * key property that makes strong local digital signals pass through
 * untouched — if the signal is already in a good range, gain stays at
 * whatever it currently is (typically 0 dB) and samples are unchanged.
 *
 * Signal chain for DIGITAL profile (per block):
 *
 *   input samples
 *       │
 *       ▼
 *   [1] Impulse Blanker   – zeros samples whose magnitude exceeds
 *                           AGC_DIGITAL_BLANKER_THRESHOLD before the
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
 *
 * Signal chain for DX / LOCAL profiles (unchanged):
 *
 *   input samples → agc_crcf (liquid-dsp) → output samples
 */

#include "agc.h"
#include "constants.h"
#include "log.h"
#include "utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <liquid.h>

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
 *
 * Prevents impulse noise spikes from corrupting the block RMS estimate
 * before it enters the Harris gain loop. Hard-zeroing is intentional —
 * blanked durations are typically 1-3 samples, far shorter than the AGC
 * time constant, so the brief discontinuity is far less harmful than a
 * gain lurch caused by a corrupted RMS measurement.
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
 *
 * RMS is computed over sample magnitudes, referenced to full scale
 * (0 dBFS = magnitude 1.0). Returns -120 dBFS for silence.
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
 *
 * Measures block RMS, computes error from target, applies deadband and
 * gain rails, updates gain via the LMS equation, then applies the
 * resulting linear gain scalar to all samples in-place.
 *
 * When the signal is inside the deadband the error is zeroed, gain_db
 * and gain_linear are not updated, and the multiply-by-gain_linear loop
 * is still executed but with an unchanged scalar (1.0x if gain_db == 0).
 */
static void harris_agc_execute(HarrisAgc    *h,
                                ComplexFloat *samples,
                                unsigned int  num_samples)
{
    /* Measure block RMS in dBFS — this is x(n) in Harris notation,
     * the signal level arriving at the input of the gain stage. */
    float rms_db    = measure_rms_dbfs(samples, num_samples);

    /* y(n) = x(n) + g(n)  — estimated output level.
     * e(n) = R - y(n)     — deviation from target. */
    float output_db = rms_db + h->gain_db;
    float error     = h->target_db - output_db;

    /* Deadband: freeze gain when signal is already close enough to target.
     * This is what makes strong local signals pass through untouched. */
    if (fabsf(error) <= h->deadband_db) {
        error = 0.0f;
    }

    /* Gain rails: don't try to adjust beyond the hard limits. */
    if (h->gain_db >= h->gain_max_db && error > 0.0f) error = 0.0f;
    if (h->gain_db <= h->gain_min_db && error < 0.0f) error = 0.0f;

    /* LMS update: g(n+1) = g(n) + alpha * e(n).
     * Only recompute the linear scalar when gain actually changes. */
    if (error != 0.0f) {
        h->gain_db += h->alpha * error;

        if (h->gain_db > h->gain_max_db) h->gain_db = h->gain_max_db;
        if (h->gain_db < h->gain_min_db) h->gain_db = h->gain_min_db;

        h->gain_linear = powf(10.0f, h->gain_db / 20.0f);
    }

    /* Apply linear gain to all samples.
     * When in deadband with gain_db == 0 this is multiply-by-one. */
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
        app->dsp.agc.object        = NULL;
        app->dsp.agc.harris_object = NULL;
        return true;
    }

    /* Initialise common runtime state. */
    app->dsp.agc.current_gain   = 1.0f;
    app->dsp.agc.samples_seen   = 0;
    app->dsp.agc.object         = NULL;
    app->dsp.agc.harris_object  = NULL;

    /* ------------------------------------------------------------------
     * DIGITAL profile — Harris/LMS, no liquid-dsp.
     * ----------------------------------------------------------------- */
    if (config->dsp.agc.profile == AGC_PROFILE_DIGITAL) {

        HarrisAgc *h = (HarrisAgc *)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(HarrisAgc), true);
        if (!h) {
            log_fatal("AGC: Failed to allocate Harris AGC state.");
            return false;
        }

        h->target_db    = AGC_DIGITAL_HARRIS_TARGET_DBFS;
        h->deadband_db  = AGC_DIGITAL_HARRIS_DEADBAND_DB;
        h->alpha        = AGC_DIGITAL_HARRIS_ALPHA;
        h->gain_min_db  = AGC_DIGITAL_HARRIS_GAIN_MIN_DB;
        h->gain_max_db  = AGC_DIGITAL_HARRIS_GAIN_MAX_DB;
        h->gain_db      = 0.0f;  /* Start at unity — do nothing until signal seen. */
        h->gain_linear  = 1.0f;
        h->samples_seen = 0;

        // The default is set by the constants file. No complex logic needed here.
        if (config->dsp.agc.target_level_arg > 0.0f) {
            h->target_db = 20.0f * log10f(config->dsp.agc.target_level_arg);
        }

        app->dsp.agc.harris_object = (struct harris_agc_s *)h;

        log_info("AGC: Enabled with [Digital] profile (Harris/LMS).");
        log_info("AGC:   Algorithm:  Harris/LMS, dB domain, block-level.");
        log_info("AGC:   Target:     %.1f dBFS", h->target_db);
        log_info("AGC:   Deadband:   ±%.1f dB  (pass-through window: [%.1f, %.1f] dBFS)",
                 h->deadband_db,
                 h->target_db - h->deadband_db,
                 h->target_db + h->deadband_db);
        log_info("AGC:   Alpha:      %.2f", h->alpha);
        log_info("AGC:   Gain range: [%.1f dB .. %.1f dB]",
                 h->gain_min_db, h->gain_max_db);
        log_info("AGC:   Blanker:    threshold magnitude > %.1f",
                 (double)AGC_DIGITAL_BLANKER_THRESHOLD);

        return true;
    }

    /* ------------------------------------------------------------------
     * DX / LOCAL profiles — liquid-dsp agc_crcf (unchanged).
     * ----------------------------------------------------------------- */
    agc_crcf q = agc_crcf_create();
    if (!q) {
        log_fatal("AGC: Failed to create liquid-dsp AGC object.");
        return false;
    }

    float bandwidth    = AGC_LOCAL_BANDWIDTH;
    float target_level = AGC_LOCAL_TARGET;
    const char *profile_name = "Unknown";

    switch (config->dsp.agc.profile) {
        case AGC_PROFILE_DX:
            bandwidth    = AGC_DX_BANDWIDTH;
            target_level = AGC_DX_TARGET;
            profile_name = "DX";
            break;
        case AGC_PROFILE_LOCAL:
            bandwidth    = AGC_LOCAL_BANDWIDTH;
            target_level = AGC_LOCAL_TARGET;
            profile_name = "Local";
            break;
        default:
            log_error("AGC: Unknown profile %d.", config->dsp.agc.profile);
            agc_crcf_destroy(q);
            return false;
    }

    // Set default target level based on profile BEFORE checking for user override
    switch (config->dsp.agc.profile) {
        case AGC_PROFILE_LOCAL:
            target_level = AGC_LOCAL_TARGET;
            break;
        case AGC_PROFILE_DX:
        default:
            target_level = AGC_DX_TARGET;
            break;
    }

    if (config->dsp.agc.target_level_arg > 0.0f) {
        target_level = config->dsp.agc.target_level_arg;
    }

    agc_crcf_set_bandwidth(q, bandwidth);
    agc_crcf_set_signal_level(q, target_level);
    agc_crcf_set_gain(q, 1.0f);

    app->dsp.agc.object = (struct liquid_agc_s *)q;

    log_info("AGC: Enabled with [%s] profile (liquid-dsp).", profile_name);
    log_info("AGC:   Bandwidth:    %.1e  (loop filter attack/release speed)", bandwidth);
    log_info("AGC:   Target level: %.2f (%.1f dBFS RMS)",
             target_level, 20.0f * log10f(target_level));

    return true;
}

void agc_apply(DspContext *dsp, ComplexFloat *samples, unsigned int num_samples)
{
    if (!dsp->config->dsp.agc.enable || num_samples == 0) return;

    /* ------------------------------------------------------------------
     * DIGITAL profile — Harris/LMS path.
     * ----------------------------------------------------------------- */
    if (dsp->config->dsp.agc.profile == AGC_PROFILE_DIGITAL) {

        HarrisAgc *h = (HarrisAgc *)dsp->agc.harris_object;
        if (!h) return;

        /* Stage 1: Impulse blanker — must run before RMS measurement. */
        apply_impulse_blanker(samples, num_samples, AGC_DIGITAL_BLANKER_THRESHOLD);

        /* Capture pre-execute gain for deadband detection in the log. */
        float gain_db_before = h->gain_db;

        /* Stage 2: Harris/LMS — measure, update, apply. */
        harris_agc_execute(h, samples, num_samples);

        /* Log on first block. */
        if (h->samples_seen == num_samples) {
            log_info("AGC: First block - gain: %.2f dB (%.4fx).",
                     h->gain_db, h->gain_linear);
        }

        /* Periodic status. */
        uint64_t prev   = dsp->agc.samples_seen;
        dsp->agc.samples_seen += num_samples;
        uint64_t period = (uint64_t)(dsp->config->output_rate.target_rate
                                     * AGC_LOG_INTERVAL_SEC);

        if (period > 0 && (prev / period) != (dsp->agc.samples_seen / period)) {
            bool in_deadband = (fabsf(h->gain_db - gain_db_before) < 1e-6f);
            log_debug("AGC: gain=%.2f dB  %s",
                      h->gain_db,
                      in_deadband ? "(deadband — passing through unchanged)"
                                  : "(adjusting)");
        }

        dsp->agc.current_gain = h->gain_linear;
        return;
    }

    /* ------------------------------------------------------------------
     * DX / LOCAL profiles — liquid-dsp path (unchanged).
     * ----------------------------------------------------------------- */
    agc_crcf q = (agc_crcf)dsp->agc.object;
    if (!q) return;

    agc_crcf_execute_block(q,
                           (liquid_float_complex *)samples,
                           num_samples,
                           (liquid_float_complex *)samples);

    if (dsp->agc.samples_seen == 0) {
        float gain = agc_crcf_get_gain(q);
        log_info("AGC: Initial gain: %.4fx (%.1f dB).",
                 gain, 20.0f * log10f(gain > 0.0f ? gain : 1e-9f));
    }

    uint64_t prev = dsp->agc.samples_seen;
    dsp->agc.samples_seen += num_samples;
    uint64_t period = (uint64_t)(dsp->config->output_rate.target_rate
                                 * AGC_LOG_INTERVAL_SEC);

    if (period > 0 && (prev / period) != (dsp->agc.samples_seen / period)) {
        float gain = agc_crcf_get_gain(q);
        float rssi = agc_crcf_get_rssi(q);
        log_debug("AGC: gain=%.2f dB  RSSI=%.1f dBFS",
                  20.0f * log10f(gain > 0.0f ? gain : 1e-9f), rssi);
    }
}

void agc_reset(DspContext *dsp)
{
    if (dsp->config->dsp.agc.profile == AGC_PROFILE_DIGITAL) {
        HarrisAgc *h = (HarrisAgc *)dsp->agc.harris_object;
        if (h) {
            h->gain_db      = 0.0f;
            h->gain_linear  = 1.0f;
            h->samples_seen = 0;
        }
    } else {
        if (dsp->agc.object) {
            agc_crcf q = (agc_crcf)dsp->agc.object;
            agc_crcf_reset(q);
            agc_crcf_set_gain(q, 1.0f);
        }
    }

    dsp->agc.current_gain  = 1.0f;
    dsp->agc.samples_seen  = 0;
}


void agc_destroy(AppContext *app)
{
    if (app->dsp.agc.harris_object) {
        // Memory handled by arena
        app->dsp.agc.harris_object = NULL;
    }

    if (app->dsp.agc.object) {
        agc_crcf_destroy((agc_crcf)app->dsp.agc.object);
        app->dsp.agc.object = NULL;
    }
}


int agc_populate_cli_options(struct argparse_option *buffer,
                              struct AppConfig       *config)
{
    struct argparse_option options[] = {
        OPT_GROUP("Output Automatic Gain Control"),
        OPT_BOOLEAN(0, "output-agc",  &config->dsp.agc.enable,
                    "Enable automatic gain control on the output.",
                    NULL, 0, 0),
        OPT_STRING( 0, "agc-profile", &config->dsp.agc.profile_str_arg,
                    "AGC profile {dx|local|digital}. (Default: local)",
                    NULL, 0, 0),
        OPT_FLOAT(  0, "agc-target",  &config->dsp.agc.target_level_arg,
                    "AGC target magnitude (0.0 - 1.0). (Default: profile dependent)",
                    NULL, 0, 0),
    };

    size_t count = sizeof(options) / sizeof(options[0]);
    memcpy(buffer, options, sizeof(options));
    return (int)count;
}

const char* agc_get_profile_name(AgcProfile profile) {
    switch (profile) {
        case AGC_PROFILE_DX:      return "DX";
        case AGC_PROFILE_LOCAL:   return "Local";
        case AGC_PROFILE_DIGITAL: return "Digital";
        default:                  return "Unknown";
    }
}

bool agc_validate_options(AppConfig *config) {
    if (config->dsp.agc.enable) {
        // 1. Validate Profile
        if (!config->dsp.agc.profile_str_arg) {
            config->dsp.agc.profile = AGC_PROFILE_LOCAL; // Default
        } else {
            if (strcasecmp(config->dsp.agc.profile_str_arg, "dx") == 0) {
                config->dsp.agc.profile = AGC_PROFILE_DX;
            } else if (strcasecmp(config->dsp.agc.profile_str_arg, "local") == 0) {
                config->dsp.agc.profile = AGC_PROFILE_LOCAL;
            } else if (strcasecmp(config->dsp.agc.profile_str_arg, "digital") == 0) {
                config->dsp.agc.profile = AGC_PROFILE_DIGITAL;
            } else {
                log_error("Invalid AGC profile '%s'. Must be 'dx', 'local', or 'digital'.", config->dsp.agc.profile_str_arg);
                return false;
            }
        }

        // 2. Validate Target Level
        if (config->dsp.agc.target_level_arg != 0.0f) {
            if (config->dsp.agc.target_level_arg <= 0.0f || config->dsp.agc.target_level_arg > 1.0f) {
                log_error("Invalid AGC target level %.2f. Must be between 0.0 and 1.0.", config->dsp.agc.target_level_arg);
                return false;
            }
            config->dsp.agc.target_level = config->dsp.agc.target_level_arg;
        }

        // 3. Check for Conflicts
        if (config->dsp.raw_passthrough) {
            log_error("Option --output-agc cannot be used with --raw-passthrough.");
            return false;
        }

        // Conflict check for Output Gain
        if (config->dsp.output_gain != 1.0f) {
            log_error("Conflicting options: --output-agc and --output-gain-multiplier cannot be used together.");
            return false;
        }

        // 4. Check for Warnings
        if (config->dsp.input_gain_provided && config->dsp.input_gain != 1.0f) {
            log_warn("Both --input-gain-multiplier and --output-agc are set.");
            log_warn("Manual gain is applied at input, but AGC will override the final volume at output.");
        }
    }
    return true;
}
