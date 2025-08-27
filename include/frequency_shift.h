/**
 * @file frequency_shift.h
 * @brief Defines the interface for the frequency shifting (NCO) module.
 *
 * This module provides functionality to apply a frequency shift to the I/Q
 * data stream. It uses a Numerically-Controlled Oscillator (NCO) from the
 * liquid-dsp library to perform the complex multiplication required for the
 * shift. It can create shifters for both the pre-resample and post-resample
 * stages of the pipeline.
 */

#ifndef FREQUENCY_SHIFT_H_
#define FREQUENCY_SHIFT_H_

#include <stdbool.h>
#include "app_context.h"   // Provides AppConfig, AppResources, and complex_float_t

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

// --- Function Declarations ---

/**
 * @brief Creates and configures the NCOs (frequency shifters) based on user arguments.
 *
 * This function reads the frequency shift settings from the AppConfig struct,
 * calculates the required shift, and creates the liquid-dsp NCO objects if a
 * shift is necessary. The created objects are stored in the AppResources struct.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources where the NCOs will be stored.
 * @return true on success or if no shift is needed, false on failure.
 */
bool freq_shift_create_ncos(AppConfig *config, AppResources *resources);

/**
 * @brief Applies the frequency shift to a block of complex samples using a specific NCO.
 *
 * @param nco The NCO object to use for the shift.
 * @param shift_hz The frequency shift in Hz (positive for up-shift, negative for down-shift).
 * @param input_buffer The source buffer of complex samples.
 * @param output_buffer The destination buffer for the shifted complex samples. Can be the same as input_buffer.
 * @param num_frames The number of frames (I/Q pairs) to process.
 */
void freq_shift_apply(nco_crcf nco, double shift_hz, complex_float_t* input_buffer, complex_float_t* output_buffer, unsigned int num_frames);

/**
 * @brief Resets the internal phase of a specific NCO.
 *
 * This is used to handle stream discontinuities (e.g., SDR overruns) to prevent
 * phase jumps in the output signal.
 *
 * @param nco The NCO object to reset.
 */
void freq_shift_reset_nco(nco_crcf nco);

/**
 * @brief Destroys the NCO objects if they were created.
 * @param resources Pointer to the application resources containing the NCOs.
 */
void freq_shift_destroy_ncos(AppResources *resources);


#endif // FREQUENCY_SHIFT_H_
