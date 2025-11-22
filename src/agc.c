/**
 * @file agc.c
 * @brief Implements the Output Automatic Gain Control module.
 *        Handles both RMS-based tracking (liquid-dsp) and Peak-based locking (custom).
 */

#include "agc.h"
#include "constants.h"
#include "log.h"
#include "utils.h" // Needed for get_monotonic_time_sec()
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
    
    // Initialize the timer
    resources->agc_last_strong_peak_time = get_monotonic_time_sec();

    // ---------------------------------------------------------
    // STRATEGY 1: DX / LOCAL (RMS Tracking via liquid-dsp)
    // ---------------------------------------------------------
    if (config->output_agc.profile != AGC_PROFILE_DIGITAL) {
        agc_crcf q = agc_crcf_create();
        if (!q) {
            log_fatal("Failed to create liquid-dsp AGC object.");
            return false;
        }

        float bandwidth = AGC_LOCAL_BANDWIDTH;
        float default_target = AGC_LOCAL_TARGET;

        if (config->output_agc.profile == AGC_PROFILE_DX) {
            bandwidth = AGC_DX_BANDWIDTH;
            default_target = AGC_DX_TARGET;
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
        
        // Peak memory not used in this mode, but safe to init
        resources->agc_peak_memory = 0.001f; 
    }
    // ---------------------------------------------------------
    // STRATEGY 2: DIGITAL (Custom Peak-Lock)
    // ---------------------------------------------------------
    else {
        // No liquid object needed for Digital mode; state is managed in AppResources
        resources->output_agc_object = NULL;

        // Start assuming a moderate signal (-26dB) to avoid massive gain spikes 
        // if the very first millisecond is pure silence/noise.
        resources->agc_peak_memory = 0.05f; 
    }

    log_info("Output AGC enabled.");

    return true;
}

void agc_apply(AppResources* resources, complex_float_t* samples, unsigned int num_samples) {
    if (!resources->config->output_agc.enable || num_samples == 0) return;

    // ---------------------------------------------------------
    // STRATEGY 1: DX / LOCAL (RMS Tracking via liquid-dsp)
    // ---------------------------------------------------------
    if (resources->output_agc_object) {
        agc_crcf_execute_block(
            (agc_crcf)resources->output_agc_object,
            (liquid_float_complex*)samples,
            num_samples,
            (liquid_float_complex*)samples
        );
        return;
    }

    // ---------------------------------------------------------
    // STRATEGY 2: DIGITAL (Peak Detect, Lock, & Slow Recovery)
    // ---------------------------------------------------------
    if (resources->config->output_agc.profile == AGC_PROFILE_DIGITAL) {
        
        // Determine Target
        float target = (resources->config->output_agc.target_level_arg > 0) 
                        ? resources->config->output_agc.target_level 
                        : AGC_DIGITAL_PEAK_TARGET;

        // =========================================================
        // PHASE A: SCANNING MODE (Startup / Look-Ahead)
        // Pass signal through while measuring peaks, but apply the
        // best-known gain immediately to avoid "quiet startup".
        // =========================================================
        if (!resources->agc_is_locked) {
            
            // 1. Look-Ahead: Find the peak in THIS block first
            float chunk_peak = 0.0f;
            for (unsigned int i = 0; i < num_samples; i++) {
                float mag = cabsf(samples[i]);
                if (mag > chunk_peak) chunk_peak = mag;
            }

            // 2. Update Global Peak Memory (Monotonic Growth)
            // We only care if this block is louder than anything seen so far.
            if (chunk_peak > resources->agc_peak_memory) {
                resources->agc_peak_memory = chunk_peak;
            }

            // 3. Calculate "Running Gain"
            // This is the highest gain we can apply without clipping the peak we just found.
            // We use a small epsilon (1e-4) to prevent divide-by-zero on pure silence.
            float safe_peak = (resources->agc_peak_memory < 1e-4f) ? 1e-4f : resources->agc_peak_memory;
            float running_gain = target / safe_peak;

            // 4. Apply Immediately (Fixes the "2-second silence" issue)
            // This ensures the very first block uses the full dynamic range.
            for (unsigned int i = 0; i < num_samples; i++) {
                samples[i] *= running_gain;
            }

            // 5. Check Timer to Finalize Lock
            double sample_rate = resources->config->target_rate;
            double elapsed = (double)resources->agc_samples_seen / sample_rate;

            if (elapsed > AGC_DIGITAL_LOCK_TIME) {
                resources->agc_is_locked = true;
                resources->agc_current_gain = running_gain;
                
                // Initialize the recovery timer for the "Locked" phase
                resources->agc_last_strong_peak_time = get_monotonic_time_sec();

                log_info("AGC Locked: Peak %.4f. Final Gain %.2f (%.1f dB).", 
                         resources->agc_peak_memory,
                         resources->agc_current_gain, 
                         20.0f * log10f(resources->agc_current_gain));
            }
        }
        // =========================================================
        // PHASE B: LOCKED MODE (Maintenance)
        // Apply gain with Safety Ratchet & Slow Recovery
        // =========================================================
        else {
            float g = resources->agc_current_gain;
            
            // 1. Find Peak in this block
            float block_peak = 0.0f;
            for (unsigned int i = 0; i < num_samples; i++) {
                float mag = cabsf(samples[i]);
                if (mag > block_peak) block_peak = mag;
            }

            float output_peak = block_peak * g;
            double current_time = get_monotonic_time_sec();

            // 2. SAFETY RATCHET (Fast Attack Down)
            // If we exceed 1.0, we MUST reduce gain immediately to prevent clipping.
            if (output_peak > 1.0f) {
                float new_gain = 0.99f / block_peak;
                
                // Log only if significant change to avoid spamming
                if (g - new_gain > 0.01f) {
                    log_info("AGC: Clipping detected (Peak %.2f). Ratcheting gain down from %.2f to %.2f.", 
                             output_peak, g, new_gain);
                }
                g = new_gain;
                
                // Reset the "strong peak" timer because we just found a VERY strong peak.
                resources->agc_last_strong_peak_time = current_time;
            }
            // 3. RECOVERY LOGIC (Hang & Creep Up)
            else {
                // Is the signal "strong enough"? (e.g. > 75% of target)
                if (output_peak > (target * AGC_DIGITAL_LOWER_THRESHOLD)) {
                    // Yes, signal is healthy. Reset the hang timer.
                    resources->agc_last_strong_peak_time = current_time;
                } 
                else {
                    // Signal is weak. Check how long it has been weak.
                    double time_since_strong = current_time - resources->agc_last_strong_peak_time;
                    
                    if (time_since_strong > AGC_DIGITAL_HANG_TIME) {
                        // We have been weak for > 4 seconds. Start creeping up.
                        g *= AGC_DIGITAL_RECOVERY_RATE;
                    }
                }
            }

            // Update global state
            resources->agc_current_gain = g;

            // Apply the (possibly adjusted) gain
            for (unsigned int i = 0; i < num_samples; i++) {
                samples[i] *= g;
            }
        }

        resources->agc_samples_seen += num_samples;
        return; 
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
    resources->agc_peak_memory = 0.05f; // Reset to safe startup floor
    resources->agc_current_gain = 1.0f;
    resources->agc_last_strong_peak_time = get_monotonic_time_sec();
}

void agc_destroy(AppResources* resources) {
    if (resources->output_agc_object) {
        agc_crcf_destroy((agc_crcf)resources->output_agc_object);
        resources->output_agc_object = NULL;
    }
}
