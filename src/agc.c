/**
 * @file agc.c
 * @brief Implements the Output Automatic Gain Control module.
 *
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

bool agc_create(AppConfig* config, AppContext* app) {
    if (!config->dsp.agc.enable) {
        app->dsp.agc.object = NULL;
        return true;
    }

    // Initialize Common State
    app->dsp.agc.is_locked = false;
    app->dsp.agc.current_gain = 1.0f;
    app->dsp.agc.samples_seen = 0;
    app->dsp.agc.last_strong_peak_time = 0.0;

    // ---------------------------------------------------------
    // STRATEGY 1: DX / LOCAL (RMS Tracking via liquid-dsp)
    // ---------------------------------------------------------
    if (config->dsp.agc.profile != AGC_PROFILE_DIGITAL) {
        agc_crcf q = agc_crcf_create();
        if (!q) {
            log_fatal("Failed to create liquid-dsp AGC object.");
            return false;
        }

        float bandwidth = AGC_LOCAL_BANDWIDTH;
        float default_target = AGC_LOCAL_TARGET;

        if (config->dsp.agc.profile == AGC_PROFILE_DX) {
            bandwidth = AGC_DX_BANDWIDTH;
            default_target = AGC_DX_TARGET;
        }

        float final_target = (config->dsp.agc.target_level_arg > 0)
                           ? config->dsp.agc.target_level
                           : default_target;

        agc_crcf_set_bandwidth(q, bandwidth);
        agc_crcf_set_signal_level(q, final_target);
        agc_crcf_set_gain(q, 1.0f);

        app->dsp.agc.object = (void*)q;
    }
    // ---------------------------------------------------------
    // STRATEGY 2: DIGITAL (Adaptive Tracking)
    // ---------------------------------------------------------
    else {
        app->dsp.agc.object = NULL;
    }

    const char* profile_name = "Unknown";
    switch (config->dsp.agc.profile) {
        case AGC_PROFILE_DX:      profile_name = "DX"; break;
        case AGC_PROFILE_LOCAL:   profile_name = "Local"; break;
        case AGC_PROFILE_DIGITAL: profile_name = "Digital"; break;
        default: break;
    }
    log_info("AGC: Enabled with [%s] profile.", profile_name);
    return true;
}

void agc_apply(DspContext* dsp, ComplexFloat* samples, unsigned int num_samples) {
    if (!dsp->config->dsp.agc.enable || num_samples == 0) return;

    // --- A. ANALOG PROFILES (Liquid DSP) ---
    if (dsp->config->dsp.agc.profile != AGC_PROFILE_DIGITAL) {
        if (dsp->agc.object) {
            agc_crcf_execute_block(
                (agc_crcf)dsp->agc.object,
                (liquid_float_complex*)samples,
                num_samples,
                (liquid_float_complex*)samples
            );
        }
        dsp->agc.samples_seen += num_samples;
        return;
    }

    // --- B. DIGITAL PROFILE ---

    float block_peak = 0.0f;

    // 1. STARTUP LOGIC (Runs ONCE)
    if (dsp->agc.samples_seen == 0) {
        double block_sum = 0.0;

        for (unsigned int i = 0; i < num_samples; i++) {
            float mag = cabsf(samples[i]);
            if (mag > block_peak) block_peak = mag;
            block_sum += mag;
        }

        float block_average = (float)(block_sum / num_samples);

        if (block_peak > 1e-9f) {
            float target = (dsp->config->dsp.agc.target_level_arg > 0)
                           ? dsp->config->dsp.agc.target_level
                           : AGC_DIGITAL_PEAK_TARGET;

            float reference_level = block_peak;
            float crest_factor = (block_average > 1e-9f) ? (block_peak / block_average) : 0.0f;

            // Transient Detection Logic
            if (crest_factor > AGC_DIGITAL_CREST_FACTOR_THRESHOLD || num_samples < 5000) {
                double robust_sum = 0.0;
                unsigned int robust_count = 0;
                float exclusion_threshold = block_average * AGC_DIGITAL_ROBUST_EXCLUSION_FACTOR;

                for (unsigned int i = 0; i < num_samples; i++) {
                    float mag = cabsf(samples[i]);
                    if (mag < exclusion_threshold) {
                        robust_sum += mag;
                        robust_count++;
                    }
                }

                float robust_average = (robust_count > 0) ? (float)(robust_sum / robust_count) : block_average;
                reference_level = robust_average * AGC_DIGITAL_ROBUST_AVG_MULTIPLIER;
                if (reference_level > block_peak) reference_level = block_peak;

                log_info("AGC: Startup transient detected (CF %.1f). Calibrating to robust average.", crest_factor);
            }

            float startup_gain = target / reference_level;
            dsp->agc.current_gain = startup_gain;
            log_info("AGC: Initial Gain Multiplier: %.4f (%.1f dB).", startup_gain, 20.0f * log10f(startup_gain));
        }
    }
    // 2. RUNTIME LOGIC
    else {
        float block_max_sq = 0.0f;
        for (unsigned int i = 0; i < num_samples; i++) {
            float re = crealf(samples[i]);
            float im = cimagf(samples[i]);
            float mag2 = (re * re) + (im * im);
            if (mag2 > block_max_sq) block_max_sq = mag2;
        }
        block_peak = sqrtf(block_max_sq);
    }

    // 3. State Machine
    float target_high = (dsp->config->dsp.agc.target_level_arg > 0)
                         ? dsp->config->dsp.agc.target_level
                         : AGC_DIGITAL_PEAK_TARGET;

    float target_low = target_high * AGC_DIGITAL_STABILITY_WINDOW;
    float noise_floor_threshold = target_high * AGC_DIGITAL_NOISE_THRESHOLD;

    float current_gain = dsp->agc.current_gain;
    float projected_peak = block_peak * current_gain;

    // Calculate block duration for time-consistent slew rates
    float sample_rate = (float)dsp->config->output_rate.target_rate;
    if (sample_rate < 1.0f) sample_rate = 48000.0f;
    float block_duration = (float)num_samples / sample_rate;

    // --- CONDITION A: FAST ATTACK (Emergency Cut) ---
    if (projected_peak > (target_high * AGC_DIGITAL_SAFETY_CLAMP)) {
        // Instant cut to safe level
        current_gain = target_high / block_peak;
    }
    // --- CONDITION B: SLOW DECAY (Recovery) ---
    else if (projected_peak < target_low) {
        // Noise Gate check
        if (projected_peak > noise_floor_threshold) {
            float recovery_target = (target_high + target_low) / 2.0f;
            float desired_gain = recovery_target / block_peak;
            float error_ratio = desired_gain / current_gain;

            float adjustment_speed = AGC_DIGITAL_SLEW_RATE * block_duration;
            if (adjustment_speed > 1.0f) adjustment_speed = 1.0f;

            float step = 1.0f + ((error_ratio - 1.0f) * adjustment_speed);
            current_gain *= step;
        }
    }
    // --- CONDITION C: STABILITY WINDOW (Hold) ---

    // 4. Apply Final Gain
    for (unsigned int i = 0; i < num_samples; i++) {
        samples[i] *= current_gain;
    }

    dsp->agc.current_gain = current_gain;
    dsp->agc.samples_seen += num_samples;
}

void agc_reset(DspContext* dsp) {
    if (dsp->agc.object) {
        agc_crcf_reset((agc_crcf)dsp->agc.object);
        agc_crcf_set_gain((agc_crcf)dsp->agc.object, 1.0f);
    }
    dsp->agc.is_locked = false;
    dsp->agc.samples_seen = 0;
    dsp->agc.current_gain = 1.0f;
    dsp->agc.last_strong_peak_time = 0.0;
}

void agc_destroy(AppContext* app) {
    if (app->dsp.agc.object) {
        agc_crcf_destroy((agc_crcf)app->dsp.agc.object);
        app->dsp.agc.object = NULL;
    }
}

int agc_populate_cli_options(struct argparse_option* buffer, struct AppConfig* config) {
    struct argparse_option options[] = {
        OPT_GROUP("Output Automatic Gain Control"),
        OPT_BOOLEAN(0, "output-agc", &config->dsp.agc.enable, "Enable automatic gain control on the output.", NULL, 0, 0),
        OPT_STRING(0,  "agc-profile", &config->dsp.agc.profile_str_arg, "AGC profile {dx|local|digital}. (Default: local)", NULL, 0, 0),
        OPT_FLOAT(0,   "agc-target", &config->dsp.agc.target_level_arg, "AGC target magnitude (0.0 - 1.0). (Default: Profile Dependent)", NULL, 0, 0),
    };
    size_t count = sizeof(options) / sizeof(options[0]);
    memcpy(buffer, options, sizeof(options));
    return (int)count;
}
