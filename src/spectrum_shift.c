// spectrum_shift.c
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
 * @brief Creates and configures the NCO (frequency shifter) based on user arguments.
 */
bool shift_create_nco(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    resources->shifter_nco = NULL;
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

    if (fabs(resources->actual_nco_shift_hz) < 1e-9) {
        return true;
    }

    // MODIFIED: Read samplerate from our new generic source_info struct.
    double rate_for_nco_setup = config->shift_after_resample ? config->target_rate : (double)resources->source_info.samplerate;

    if (fabs(resources->actual_nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco_setup)) {
        log_error("Requested frequency shift %.2f Hz exceeds sanity limit for a sample rate of %.1f Hz.",
                resources->actual_nco_shift_hz, rate_for_nco_setup);
        return false;
    }

    resources->shifter_nco = nco_crcf_create(LIQUID_NCO);
    if (!resources->shifter_nco) {
        log_error("Failed to create NCO (frequency shifter).");
        return false;
    }

    float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(resources->actual_nco_shift_hz) / rate_for_nco_setup);
    nco_crcf_set_frequency(resources->shifter_nco, nco_freq_rad_per_sample);

    return true;
}

/**
 * @brief Applies the frequency shift to a block of complex samples.
 */
void shift_apply(AppConfig *config, AppResources *resources, WorkItem *item, ShiftApplyStage stage) {
    if (!resources->shifter_nco) {
        return;
    }

    bool apply_now = false;
    complex_float_t *input_buffer = NULL;
    complex_float_t *output_buffer = NULL;
    unsigned int num_frames = 0;

    if (stage == SHIFT_STAGE_PRE_RESAMPLE && !config->shift_after_resample) {
        apply_now = true;
        input_buffer = item->complex_buffer_scaled;
        output_buffer = item->complex_buffer_shifted;
        num_frames = (unsigned int)item->frames_read;
    } else if (stage == SHIFT_STAGE_POST_RESAMPLE && config->shift_after_resample) {
        apply_now = true;
        input_buffer = item->complex_buffer_resampled;
        output_buffer = item->complex_buffer_shifted;
        num_frames = item->frames_to_write;
    }

    if (apply_now && num_frames > 0) {
        if (resources->actual_nco_shift_hz >= 0) {
            nco_crcf_mix_block_up(resources->shifter_nco, input_buffer, output_buffer, num_frames);
        } else {
            nco_crcf_mix_block_down(resources->shifter_nco, input_buffer, output_buffer, num_frames);
        }
    }
}

/**
 * @brief Checks if the configured frequency shift exceeds the Nyquist frequency and warns the user.
 */
bool shift_check_nyquist_warning(const AppConfig *config, const AppResources *resources) {
    if (!config || !resources || config->output_to_stdout || fabs(resources->actual_nco_shift_hz) < 1e-9) {
        return true;
    }

    // MODIFIED: Read samplerate from our new generic source_info struct.
    double rate_for_nyquist_check = config->shift_after_resample ? config->target_rate : (double)resources->source_info.samplerate;
    double nyquist_freq = rate_for_nyquist_check / 2.0;

    if (fabs(resources->actual_nco_shift_hz) > nyquist_freq) {
        log_warn("Required frequency shift %.2f Hz exceeds the Nyquist frequency %.2f Hz for the stage where it is applied.",
                resources->actual_nco_shift_hz, nyquist_freq);
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
                log_info("Operation cancelled by user.");
                return false;
            }
        } while (response != 'y');
    }
    return true;
}

/**
 * @brief Resets the internal state of the NCO.
 */
void shift_reset_nco(AppResources *resources) {
    if (resources && resources->shifter_nco) {
        nco_crcf_reset(resources->shifter_nco);
    }
}

/**
 * @brief Destroys the NCO object if it was created.
 */
void shift_destroy_nco(AppResources *resources) {
    if (resources && resources->shifter_nco) {
        nco_crcf_destroy(resources->shifter_nco);
        resources->shifter_nco = NULL;
    }
}
