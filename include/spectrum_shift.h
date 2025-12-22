// spectrum_shift.h
#ifndef SPECTRUM_SHIFT_H_
#define SPECTRUM_SHIFT_H_

#include "types.h"
#include <stdbool.h>

/**
 * @brief Creates and configures the NCOs (frequency shifters) based on user arguments.
 *
 * This function reads the frequency shift settings from the AppConfig struct,
 * calculates the required shift, and creates the liquid-dsp NCO objects if a
 * shift is necessary. The created objects are stored in the AppContext struct.
 *
 * @param config Pointer to the application configuration.
 * @param app Pointer to the application app where the NCOs will be stored.
 * @return true on success or if no shift is needed, false on failure (e.g., metadata missing, NCO creation fails).
 */
bool freq_shift_create_ncos(AppConfig *config, AppContext* app);

/**
 * @brief Applies the frequency shift to a block of complex samples using a specific NCO.
 *
 * @param nco The NCO object to use for the shift.
 * @param shift_hz The frequency shift in Hz (positive for up-shift, negative for down-shift).
 * @param input_buffer The source buffer of complex samples.
 * @param output_buffer The destination buffer for the shifted complex samples.
 * @param num_frames The number of frames to process.
 */
void freq_shift_apply(nco_crcf nco, double shift_hz, ComplexFloat* input_buffer, ComplexFloat* output_buffer, unsigned int num_frames);

/**
 * @brief Resets the internal state of a specific NCO.
 * @param nco The NCO object to reset.
 */
void freq_shift_reset_nco(nco_crcf nco);

/**
 * @brief Destroys the NCO objects if they were created.
 * @param app Pointer to the application app containing the NCOs.
 */
void freq_shift_destroy_ncos(AppContext* app);


#endif // SPECTRUM_SHIFT_H_
