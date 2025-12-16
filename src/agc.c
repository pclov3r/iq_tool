/**
 * @file agc.c
 * @brief Implements the Output Automatic Gain Control module.
 *
 * This module provides functionality to initialize, apply, and release app
 * for an automatic gain control (AGC) object. It supports three distinct profiles:
 * 1. DX: Slow RMS tracking for weak/fading analog signals (liquid-dsp).
 * 2. Local: Fast RMS tracking for strong analog signals (liquid-dsp).
 * 3. Digital: Adaptive Tracking with Hysteresis. Designed to maximize
 *    Modulation Error Ratio (MER) by maintaining constant gain within a
 *    stability window and only adjusting for significant signal changes.
 */

#include "agc.h"
#include "constants.h"
#include "log.h"
#include "utils.h" // Needed for get_monotonic_time_sec()
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
    app->dsp.agc.last_strong_peak_time = get_monotonic_time_sec();

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

        // Use CLI target if provided, otherwise profile default
        float final_target = (config->dsp.agc.target_level_arg > 0) 
                           ? config->dsp.agc.target_level 
                           : default_target;

        agc_crcf_set_bandwidth(q, bandwidth);
        agc_crcf_set_signal_level(q, final_target);
        agc_crcf_set_gain(q, 1.0f); // Will be snapped on first apply

        app->dsp.agc.object = (void*)q;
        
        // Peak memory not used in this mode, but safe to init
        app->dsp.agc.peak_memory = 0.001f; 
    }
    // ---------------------------------------------------------
    // STRATEGY 2: DIGITAL (Adaptive Tracking)
    // ---------------------------------------------------------
    else {
        // No liquid object needed for Digital mode; state is managed in AppContext app->dsp.agc.object = NULL;

        // Initialize peak memory to a safe non-zero value to prevent divide-by-zero
        // before the first block is processed.
        app->dsp.agc.peak_memory = 0.05f; 
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

void agc_apply(DspContext* dsp, complex_float_t* samples, unsigned int num_samples) {
    if (!dsp->config->dsp.agc.enable || num_samples == 0) return;

    // 1. Common Analysis: Find Peak AND Average Energy of the current block
    float block_peak = 0.0f;
    double block_sum = 0.0; // Use double to prevent overflow

    for (unsigned int i = 0; i < num_samples; i++) {
        float mag = cabsf(samples[i]);
        if (mag > block_peak) block_peak = mag;
        block_sum += mag;
    }
    
    float block_average = (float)(block_sum / num_samples);

    // 2. Unified Startup Snap (Auto Gain)
    // If this is the first block, calculate the ideal gain to hit the target immediately.
    // We also detect outliers (transients) here to avoid setting the gain too low.
    bool skip_safety_check = false;

    if (dsp->agc.samples_seen == 0 && block_peak > 1e-9f) {
        
        float target = 0.5f; // Default safe fallback

        switch (dsp->config->dsp.agc.profile) {
            case AGC_PROFILE_DIGITAL:
                target = AGC_DIGITAL_PEAK_TARGET;
                break;
            case AGC_PROFILE_DX:
                target = AGC_DX_TARGET;
                break;
            case AGC_PROFILE_LOCAL:
                target = AGC_LOCAL_TARGET;
                break;
            default: break;
        }

        if (dsp->config->dsp.agc.target_level_arg > 0) {
            target = dsp->config->dsp.agc.target_level_arg;
        }

        // --- Outlier Rejection Logic (Two-Pass Robust Average) ---
        float reference_level = block_peak;
        bool outlier_detected = false;
        float crest_factor = (block_average > 1e-9f) ? (block_peak / block_average) : 0.0f;

        if (crest_factor > AGC_DIGITAL_CREST_FACTOR_THRESHOLD || num_samples < 5000) {
            outlier_detected = true;
            
            // PASS 2: Re-calculate average excluding high-energy samples.
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

            // Target a "Safe Peak" based on the Robust Average.
            reference_level = robust_average * AGC_DIGITAL_ROBUST_AVG_MULTIPLIER; 
            
            if (reference_level > block_peak) reference_level = block_peak;

            skip_safety_check = true;
        }

        float startup_gain = target / reference_level;

        // Apply the calculated gain
        if (dsp->config->dsp.agc.profile == AGC_PROFILE_DIGITAL) {
            dsp->agc.current_gain = startup_gain;
            dsp->agc.peak_memory = target; 
        } else {
            if (dsp->agc.object) {
                agc_crcf_set_gain((agc_crcf)dsp->agc.object, startup_gain);
            }
        }

        if (outlier_detected) {
            log_info("AGC: Startup transient detected (Crest Factor %.1f). Calibrating to robust average.",
                     crest_factor);
            log_info("AGC: Calculated Initial Gain Multiplier: %.4f (%.1f dB).",
                     startup_gain, 20.0f * log10f(startup_gain));
        } else {
            log_info("AGC: Calculated Initial Gain Multiplier: %.4f (%.1f dB).",
                     startup_gain, 20.0f * log10f(startup_gain));
        }
    }

    // 3. Runtime Logic (Profile Specific)

    // --- A. ANALOG PROFILES (DX / Local) ---
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

    // --- B. DIGITAL PROFILE (Adaptive Tracking) ---
    
    float target_high = (dsp->config->dsp.agc.target_level_arg > 0) 
                         ? dsp->config->dsp.agc.target_level 
                         : AGC_DIGITAL_PEAK_TARGET;
    float target_low = target_high * AGC_DIGITAL_STABILITY_WINDOW; 
    float noise_floor_threshold = target_high * AGC_DIGITAL_NOISE_THRESHOLD; 

    float current_gain = dsp->agc.current_gain;
    float projected_peak = block_peak * current_gain;

    if (!skip_safety_check) {
        // --- TIME-BASED ADJUSTMENT CALCULATION ---
        // Calculate the duration of this block in seconds to ensure consistent
        // attack/decay rates regardless of sample rate or buffer size.
        float sample_rate = (float)dsp->config->output_rate.target_rate;
        if (sample_rate < 1.0f) sample_rate = 48000.0f; // Sanity check
        float block_duration = (float)num_samples / sample_rate;

        // --- CONDITION A: FAST ATTACK (Emergency Cut) ---
        // RELAXED SAFETY: Allow peaks up to SAFETY_CLAMP (e.g. 3.0) before triggering a cut.
        if (projected_peak > AGC_DIGITAL_SAFETY_CLAMP) {
            float safe_gain = target_high / block_peak;
            dsp->agc.current_gain = safe_gain;
            dsp->agc.last_strong_peak_time = get_monotonic_time_sec();
        }
        // --- CONDITION B: SLOW DECAY (Recovery) ---
        else if (projected_peak < target_low) {
            if (projected_peak > noise_floor_threshold) {
                float recovery_target = (target_high + target_low) / 2.0f;
                float desired_gain = recovery_target / block_peak;
                float error_ratio = desired_gain / current_gain;
                
                // Scale the slew rate by the block duration (Time-Based Tracking)
                float adjustment_speed = AGC_DIGITAL_SLEW_RATE * block_duration;
                if (adjustment_speed > 1.0f) adjustment_speed = 1.0f; // Clamp to max 100% per block

                float step = 1.0f + ((error_ratio - 1.0f) * adjustment_speed); 
                
                dsp->agc.current_gain *= step;
                dsp->agc.last_strong_peak_time = get_monotonic_time_sec();
            }
        }
        // --- CONDITION C: STABILITY WINDOW (Hold) ---
        else {
            dsp->agc.last_strong_peak_time = get_monotonic_time_sec();
        }
    }

    // 4. Apply Final Gain
    float g = dsp->agc.current_gain;
    for (unsigned int i = 0; i < num_samples; i++) {
        samples[i] *= g;
    }

    dsp->agc.samples_seen += num_samples;
}

void agc_reset(DspContext* dsp) {
    // Reset DX/Local state
    if (dsp->agc.object) {
        agc_crcf_reset((agc_crcf)dsp->agc.object);
        agc_crcf_set_gain((agc_crcf)dsp->agc.object, 1.0f);
    }

    // Reset Digital state
    dsp->agc.is_locked = false;
    dsp->agc.samples_seen = 0;
    dsp->agc.peak_memory = 0.05f; // Reset to safe startup floor
    dsp->agc.current_gain = 1.0f;
    dsp->agc.last_strong_peak_time = get_monotonic_time_sec();
}

void agc_destroy(AppContext* app) {
    if (app->dsp.agc.object) {
        agc_crcf_destroy((agc_crcf)app->dsp.agc.object);
        app->dsp.agc.object = NULL;
    }
}

int agc_populate_cli_options(struct argparse_option* buffer, struct AppConfig* config) {
    struct argparse_option options[] = {
        OPT_GROUP("Output Automatic Gain Control (AGC)"),
        OPT_BOOLEAN(0, "output-agc", &config->dsp.agc.enable, "Enable automatic gain control on the output.", NULL, 0, 0),
        OPT_STRING(0,  "agc-profile", &config->dsp.agc.profile_str_arg, "AGC profile {dx|local|digital}. (Default: local)", NULL, 0, 0),
        OPT_FLOAT(0,   "agc-target", &config->dsp.agc.target_level_arg, "AGC target magnitude (0.0 - 1.0). (Default: Profile Dependent)", NULL, 0, 0),
    };

    size_t count = sizeof(options) / sizeof(options[0]);
    memcpy(buffer, options, sizeof(options));
    return (int)count;
}
