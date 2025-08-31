#include "frequency_shift.h"
#include "constants.h"
#include "app_context.h" // Provides AppConfig, AppResources
#include "utils.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Creates and configures the NCOs (frequency shifters) based on user arguments.
 */
bool freq_shift_create_ncos(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    resources->pre_resample_nco = NULL;
    resources->post_resample_nco = NULL;

    // If no shift is needed, we're done.
    if (fabs(resources->nco_shift_hz) < 1e-9) {
        return true;
    }

    // --- Create Pre-Resample NCO ---
    if (!config->shift_after_resample) {
        double rate_for_nco = (double)resources->source_info.samplerate;
        if (fabs(resources->nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco)) {
            log_error("Requested frequency shift %.2f Hz exceeds sanity limit for the pre-resample rate of %.1f Hz.", resources->nco_shift_hz, rate_for_nco);
            return false;
        }
        resources->pre_resample_nco = nco_crcf_create(LIQUID_NCO);
        if (!resources->pre_resample_nco) {
            log_error("Failed to create pre-resample NCO (frequency shifter).");
            return false;
        }
        float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(resources->nco_shift_hz) / rate_for_nco);
        nco_crcf_set_frequency(resources->pre_resample_nco, nco_freq_rad_per_sample);
    }

    // --- Create Post-Resample NCO ---
    if (config->shift_after_resample) {
        double rate_for_nco = config->target_rate;
         if (fabs(resources->nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco)) {
            log_error("Requested frequency shift %.2f Hz exceeds sanity limit for the post-resample rate of %.1f Hz.", resources->nco_shift_hz, rate_for_nco);
            return false;
        }
        resources->post_resample_nco = nco_crcf_create(LIQUID_NCO);
        if (!resources->post_resample_nco) {
            log_error("Failed to create post-resample NCO (frequency shifter).");
            freq_shift_destroy_ncos(resources); // Clean up pre-resample NCO if it was created
            return false;
        }
        float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(resources->nco_shift_hz) / rate_for_nco);
        nco_crcf_set_frequency(resources->post_resample_nco, nco_freq_rad_per_sample);
    }

    return true;
}

/**
 * @brief Applies the frequency shift to a block of complex samples using a specific NCO.
 */
void freq_shift_apply(nco_crcf nco, double shift_hz, complex_float_t* input_buffer, complex_float_t* output_buffer, unsigned int num_frames) {
    if (!nco || num_frames == 0) {
        return;
    }

    if (shift_hz >= 0) {
        nco_crcf_mix_block_up(nco, input_buffer, output_buffer, num_frames);
    } else {
        nco_crcf_mix_block_down(nco, input_buffer, output_buffer, num_frames);
    }
}

/**
 * @brief Resets the NCO's phase accumulator without destroying its frequency.
 * This is the safe way to handle stream discontinuities from SDRs.
 */
void freq_shift_reset_nco(nco_crcf nco) {
    if (nco) {
        // This only resets the phase, leaving the frequency configuration intact.
        nco_crcf_set_phase(nco, 0.0f);
    }
}

/**
 * @brief Destroys the NCO objects if they were created.
 */
void freq_shift_destroy_ncos(AppResources *resources) {
    if (resources) {
        if (resources->pre_resample_nco) {
            nco_crcf_destroy(resources->pre_resample_nco);
            resources->pre_resample_nco = NULL;
        }
        if (resources->post_resample_nco) {
            nco_crcf_destroy(resources->post_resample_nco);
            resources->post_resample_nco = NULL;
        }
    }
}
