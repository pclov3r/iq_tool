#include "frequency_shift.h"
#include "constants.h"
#include "app_context.h"
#include "utilities.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <liquid.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Creates and configures the NCOs (frequency shifters) based on user arguments.
 */
bool frequency_shift_create(AppConfig *config, AppContext* app) {
    if (!config || !app) return false;

    app->dsp.pre_resample_nco = NULL;
    app->dsp.post_resample_nco = NULL;

    // First, resolve the final shift value. If a module (like WAV) hasn't already
    // calculated a shift, check for the generic manual shift option from the CLI.
    if (app->dsp.nco_shift_hz == 0.0 && config->dsp.frequency_shift_hz != 0.0f) {
        app->dsp.nco_shift_hz = config->dsp.frequency_shift_hz;
    }

    // Now that the final shift value is resolved, validate dependent options.
    if (config->dsp.shift_after_resample && fabs(app->dsp.nco_shift_hz) < 1e-9) {
        log_error("Option --shift-after-resample was used, but no effective frequency shift was requested or calculated.");
        return false;
    }

    // If no shift is needed, we're done.
    if (fabs(app->dsp.nco_shift_hz) < 1e-9) {
        return true;
    }

    // --- Create Pre-Resample NCO ---
    if (!config->dsp.shift_after_resample) {
        double rate_for_nco = (double)app->module.source_info.sample_rate;
        if (fabs(app->dsp.nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco)) {
            log_error("Requested frequency shift %.15g Hz exceeds sanity limit for the pre-resample rate of %.15g Hz.", app->dsp.nco_shift_hz, rate_for_nco);
            return false;
        }
        app->dsp.pre_resample_nco = (struct freq_shifter_s*)nco_crcf_create(LIQUID_NCO);
        if (!app->dsp.pre_resample_nco) {
            log_error("Failed to create pre-resample NCO (frequency shifter).");
            return false;
        }
        float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(app->dsp.nco_shift_hz) / rate_for_nco);
        nco_crcf_set_frequency((nco_crcf)app->dsp.pre_resample_nco, nco_freq_rad_per_sample);
    }

    // --- Create Post-Resample NCO ---
    if (config->dsp.shift_after_resample) {
        double rate_for_nco = config->output_sample_rate.rate_hz;
         if (fabs(app->dsp.nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco)) {
            log_error("Requested frequency shift %.15g Hz exceeds sanity limit for the post-resample rate of %.15g Hz.", app->dsp.nco_shift_hz, rate_for_nco);
            return false;
        }
        app->dsp.post_resample_nco = (struct freq_shifter_s*)nco_crcf_create(LIQUID_NCO);
        if (!app->dsp.post_resample_nco) {
            log_error("Failed to create post-resample NCO (frequency shifter).");
            frequency_shift_destroy_ncos(app); // Clean up pre-resample NCO if it was created
            return false;
        }
        float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(app->dsp.nco_shift_hz) / rate_for_nco);
        nco_crcf_set_frequency((nco_crcf)app->dsp.post_resample_nco, nco_freq_rad_per_sample);
    }

    return true;
}

/**
 * @brief Applies the frequency shift to a block of complex samples using a specific NCO.
 */
void frequency_shift_apply(FreqShifter* nco, double shift_hz, ComplexFloat* input_buffer, ComplexFloat* output_buffer, unsigned int num_frames) {
    if (!nco || num_frames == 0) {
        return;
    }

    if (shift_hz >= 0) {
        nco_crcf_mix_block_up((nco_crcf)nco, (liquid_float_complex*)input_buffer, (liquid_float_complex*)output_buffer, num_frames);
    } else {
        nco_crcf_mix_block_down((nco_crcf)nco, (liquid_float_complex*)input_buffer, (liquid_float_complex*)output_buffer, num_frames);
    }
}

/**
 * @brief Resets the NCO's phase accumulator without destroying its frequency.
 * This is the safe way to handle stream discontinuities from SDRs.
 */
void frequency_shift_reset_nco(FreqShifter* nco) {
    if (nco) {
        // This only resets the phase, leaving the frequency configuration intact.
        nco_crcf_set_phase((nco_crcf)nco, 0.0f);
    }
}

/**
 * @brief Destroys the NCO objects if they were created.
 */
void frequency_shift_destroy_ncos(AppContext* app) {
    if (app) {
        if (app->dsp.pre_resample_nco) {
            nco_crcf_destroy((nco_crcf)app->dsp.pre_resample_nco);
            app->dsp.pre_resample_nco = NULL;
        }
        if (app->dsp.post_resample_nco) {
            nco_crcf_destroy((nco_crcf)app->dsp.post_resample_nco);
            app->dsp.post_resample_nco = NULL;
        }
    }
}
