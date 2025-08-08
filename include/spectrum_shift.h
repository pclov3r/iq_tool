#ifndef SPECTRUM_SHIFT_H_
#define SPECTRUM_SHIFT_H_

#include "types.h"
#include <stdbool.h>

/**
 * @brief Creates and configures the NCOs (frequency shifters) based on user arguments.
 *
 * This function reads the frequency shift settings from the AppConfig struct,
 * calculates the required shift, and creates the liquid-dsp NCO objects if a
 * shift is necessary. The created objects are stored in the AppResources struct.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources where the NCOs will be stored.
 * @return true on success or if no shift is needed, false on failure (e.g., metadata missing, NCO creation fails).
 */
bool shift_create_ncos(AppConfig *config, AppResources *resources);

/**
 * @brief Applies the frequency shift to a block of complex samples using a specific NCO.
 *
 * @param nco The NCO object to use for the shift.
 * @param shift_hz The frequency shift in Hz (positive for up-shift, negative for down-shift).
 * @param input_buffer The source buffer of complex samples.
 * @param output_buffer The destination buffer for the shifted complex samples.
 * @param num_frames The number of frames to process.
 */
void shift_apply(nco_crcf nco, double shift_hz, complex_float_t* input_buffer, complex_float_t* output_buffer, unsigned int num_frames);

/**
 * @brief Checks if the configured frequency shift exceeds the Nyquist frequency and warns the user.
 *
 * If a large shift is requested, this function prints a warning about potential aliasing
 * and prompts the user to continue, unless outputting to stdout.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @return true if the user chooses to continue or no warning is needed, false if the user cancels.
 */
bool shift_check_nyquist_warning(const AppConfig *config, const AppResources *resources);

/**
 * @brief Resets the internal state of a specific NCO.
 * @param nco The NCO object to reset.
 */
void shift_reset_nco(nco_crcf nco);

/**
 * @brief Destroys the NCO objects if they were created.
 * @param resources Pointer to the application resources containing the NCOs.
 */
void shift_destroy_ncos(AppResources *resources);


#endif // SPECTRUM_SHIFT_H_
