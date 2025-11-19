/**
 * @file agc.c
 * @brief Implements the Output Automatic Gain Control module.
 *        Handles both RMS-based tracking (liquid-dsp) and Peak-based locking (custom).
 */

#include "agc.h"
#include "constants.h"
#include "log.h"
#include <stdlib.h>
#include <stdio.h> // For snprintf
#include <math.h>

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

bool agc_create(AppConfig* config, AppResources* resources) {
    if (!config->output_agc.enable) {
        resources->output_agc_object = NULL;
        return true;
    }

    // Initialize Common State
    resources->agc_is_locked = false;
    resources->agc_current_gain = 1.0f;
    resources->agc_samples_seen = 0;
    resources->agc_peak_memory = 0.001f; // Avoid divide-by-zero

    const char* profile_name = "Unknown";
    char config_summary[128];

    // --- Strategy 1: Custom Peak-Lock (Digital) ---
    if (config->output_agc.profile == AGC_PROFILE_DIGITAL) {
        resources->output_agc_object = NULL; // No liquid object needed
        profile_name = "Digital (Peak-Lock)";
        
        float target = (config->output_agc.target_level_arg > 0) 
                     ? config->output_agc.target_level 
                     : AGC_DIGITAL_PEAK_TARGET;

        snprintf(config_summary, sizeof(config_summary), 
                 "ScanTime=%.1fs, PeakTarget=%.2f", 
                 (double)AGC_DIGITAL_LOCK_TIME, target);
    }
    // --- Strategy 2: RMS Tracking (DX / Local) ---
    else {
        agc_crcf q = agc_crcf_create();
        if (!q) {
            log_fatal("Failed to create liquid-dsp AGC object.");
            return false;
        }

        float bandwidth = AGC_LOCAL_BANDWIDTH;
        float default_target = AGC_LOCAL_TARGET;
        profile_name = "Local (Fast RMS)";

        if (config->output_agc.profile == AGC_PROFILE_DX) {
            bandwidth = AGC_DX_BANDWIDTH;
            default_target = AGC_DX_TARGET;
            profile_name = "DX (Slow RMS)";
        }

        // Use CLI target if provided, otherwise profile default
        float final_target = (config->output_agc.target_level_arg > 0) 
                           ? config->output_agc.target_level 
                           : default_target;

        agc_crcf_set_bandwidth(q, bandwidth);
        agc_crcf_set_signal_level(q, final_target);
        
        // Initialize gain to unity to prevent startup noise bursts
        agc_crcf_set_gain(q, 1.0f);

        resources->output_agc_object = (void*)q;

        snprintf(config_summary, sizeof(config_summary), 
                 "Bandwidth=%.1e, RMSTarget=%.2f", 
                 bandwidth, final_target);
    }

    log_info("Output AGC enabled. Profile: %s", profile_name);

    return true;
}

void agc_apply(AppResources* resources, complex_float_t* samples, unsigned int num_samples) {
    if (!resources->config->output_agc.enable || num_samples == 0) return;

    // ---------------------------------------------------------
    // STRATEGY 1: DIGITAL (Peak Detect & Lock)
    // ---------------------------------------------------------
    if (resources->config->output_agc.profile == AGC_PROFILE_DIGITAL) {
        
        // Phase A: Locked - Apply gain with Soft Safety Ratchet
        if (resources->agc_is_locked) {
            float g = resources->agc_current_gain;
            
            // Optimization: Check peaks in this block
            float block_peak = 0.0f;
            for (unsigned int i = 0; i < num_samples; i++) {
                float mag = cabsf(samples[i]);
                if (mag > block_peak) block_peak = mag;
            }

            // SOFT SAFETY RATCHET:
            // Only trigger if we actually exceed 1.0 (Hard Clipping).
            // Instead of crushing the gain to fit the spike, we just nudge it down by 5%.
            if (block_peak * g > 1.0f) {
                float new_gain = g * 0.95f; // Reduce by 5%
                
                // Log only if significant change to avoid spamming
                if (g - new_gain > 0.01f) {
                    // CHANGED: log_warn -> log_info
                    log_info("Digital AGC: Clipping detected (Peak %.2f). Nudging gain down from %.2f to %.2f.", 
                             block_peak * g, g, new_gain);
                }
                
                g = new_gain;
                resources->agc_current_gain = g; // Update global state
            }

            // Apply the (possibly ratcheted) gain
            for (unsigned int i = 0; i < num_samples; i++) {
                samples[i] *= g;
            }
            return;
        }

        // Phase B: Scanning
        // Pass signal through at unity gain (1.0) while measuring peaks.
        
        float chunk_peak = 0.0f;
        for (unsigned int i = 0; i < num_samples; i++) {
            float mag = cabsf(samples[i]);
            if (mag > chunk_peak) chunk_peak = mag;
        }

        if (chunk_peak > resources->agc_peak_memory) {
            resources->agc_peak_memory = chunk_peak;
        }

        // Check Timer
        double sample_rate = resources->config->target_rate;
        double elapsed = (double)resources->agc_samples_seen / sample_rate;

        if (elapsed > AGC_DIGITAL_LOCK_TIME) {
            resources->agc_is_locked = true;
            
            // Determine Target
            float target = (resources->config->output_agc.target_level_arg > 0) 
                         ? resources->config->output_agc.target_level 
                         : AGC_DIGITAL_PEAK_TARGET;

            // Calculate safe gain: Target / Max_Peak
            if (resources->agc_peak_memory < 1e-4f) resources->agc_peak_memory = 1e-4f;
            resources->agc_current_gain = target / resources->agc_peak_memory;

            log_info("AGC Locked: Max Peak %.4f. Applied Gain %.2f (%.1f dB).", 
                     resources->agc_peak_memory,
                     resources->agc_current_gain, 
                     20.0f * log10f(resources->agc_current_gain));
        }

        resources->agc_samples_seen += num_samples;
        return; 
    }

    // ---------------------------------------------------------
    // STRATEGY 2: DX / LOCAL (RMS Tracking via liquid-dsp)
    // ---------------------------------------------------------
    if (resources->output_agc_object) {
        agc_crcf_execute_block(
            (agc_crcf)resources->output_agc_object,
            (liquid_float_complex*)samples,
            num_samples,
            (liquid_float_complex*)samples
        );
    }
}

void agc_reset(AppResources* resources) {
    // Reset DX/Local state
    if (resources->output_agc_object) {
        agc_crcf_reset((agc_crcf)resources->output_agc_object);
        agc_crcf_set_gain((agc_crcf)resources->output_agc_object, 1.0f);
    }

    // Reset Digital state
    resources->agc_is_locked = false;
    resources->agc_samples_seen = 0;
    resources->agc_peak_memory = 0.001f;
    resources->agc_current_gain = 1.0f;
}

void agc_destroy(AppResources* resources) {
    if (resources->output_agc_object) {
        agc_crcf_destroy((agc_crcf)resources->output_agc_object);
        resources->output_agc_object = NULL;
    }
}
