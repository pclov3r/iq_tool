#include "spectrum_shift.h"
#include "config.h"
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
bool shift_create_ncos(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    resources->pre_resample_nco = NULL;
    resources->post_resample_nco = NULL;

    double required_shift_hz = 0.0;
    if (config->set_center_frequency_target_hz) {
        if (!resources->sdr_info.center_freq_hz_present) {
            log_error("--target-freq provided, but input file lacks center frequency metadata.");
            return false;
        }
        required_shift_hz = resources->sdr_info.center_freq_hz - config->center_frequency_target_hz;
    } else if (config->freq_shift_requested) {
        required_shift_hz = config->freq_shift_hz;
    }

    resources->actual_nco_shift_hz = required_shift_hz;

    // If no shift is needed, we're done.
    if (fabs(resources->actual_nco_shift_hz) < 1e-9) {
        return true;
    }

    // --- Create Pre-Resample NCO ---
    if (!config->shift_after_resample) {
        double rate_for_nco = (double)resources->source_info.samplerate;
        if (fabs(resources->actual_nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco)) {
            log_error("Requested frequency shift %.2f Hz exceeds sanity limit for the pre-resample rate of %.1f Hz.", resources->actual_nco_shift_hz, rate_for_nco);
            return false;
        }
        resources->pre_resample_nco = nco_crcf_create(LIQUID_NCO);
        if (!resources->pre_resample_nco) {
            log_error("Failed to create pre-resample NCO (frequency shifter).");
            return false;
        }
        float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(resources->actual_nco_shift_hz) / rate_for_nco);
        nco_crcf_set_frequency(resources->pre_resample_nco, nco_freq_rad_per_sample);
    }

    // --- Create Post-Resample NCO ---
    if (config->shift_after_resample) {
        double rate_for_nco = config->target_rate;
         if (fabs(resources->actual_nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco)) {
            log_error("Requested frequency shift %.2f Hz exceeds sanity limit for the post-resample rate of %.1f Hz.", resources->actual_nco_shift_hz, rate_for_nco);
            return false;
        }
        resources->post_resample_nco = nco_crcf_create(LIQUID_NCO);
        if (!resources->post_resample_nco) {
            log_error("Failed to create post-resample NCO (frequency shifter).");
            shift_destroy_ncos(resources); // Clean up pre-resample NCO if it was created
            return false;
        }
        float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(resources->actual_nco_shift_hz) / rate_for_nco);
        nco_crcf_set_frequency(resources->post_resample_nco, nco_freq_rad_per_sample);
    }

    return true;
}

/**
 * @brief Applies the frequency shift to a block of complex samples using a specific NCO.
 */
void shift_apply(nco_crcf nco, double shift_hz, complex_float_t* input_buffer, complex_float_t* output_buffer, unsigned int num_frames) {
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
 * @brief Checks if the configured frequency shift exceeds the Nyquist frequency and warns the user.
 */
bool shift_check_nyquist_warning(const AppConfig *config, const AppResources *resources) {
    if (!config || !resources || config->output_to_stdout || fabs(resources->actual_nco_shift_hz) < 1e-9) {
        return true;
    }

    double rate_for_nyquist_check = config->shift_after_resample ? config->target_rate : (double)resources->source_info.samplerate;
    double nyquist_freq = rate_for_nyquist_check / 2.0;

    if (fabs(resources->actual_nco_shift_hz) > nyquist_freq) {
        log_warn("Required frequency shift %.2f Hz exceeds the Nyquist frequency %.2f Hz for the stage where it is applied.", resources->actual_nco_shift_hz, nyquist_freq);
        log_warn("This may cause aliasing and corrupt the signal.");

        int response;
        do {
            fprintf(stderr, "Continue anyway? (y/n): ");
            response = getchar();
            if (response == EOF) {
                fprintf(stderr, "\nEOF detected. Cancelling.\n");
                return false;
            }
            clear_stdin_buffer();
            response = tolower(response);
            if (response == 'n') {
                log_debug("Operation cancelled by user.");
                return false;
            }
        } while (response != 'y');
    }
    return true;
}

/**
 * @brief Resets the internal state of a specific NCO.
 */
void shift_reset_nco(nco_crcf nco) {
    if (nco) {
        nco_crcf_reset(nco);
    }
}

/**
 * @brief Destroys the NCO objects if they were created.
 */
void shift_destroy_ncos(AppResources *resources) {
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
