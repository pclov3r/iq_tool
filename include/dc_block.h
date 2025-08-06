#ifndef DC_BLOCK_H_
#define DC_BLOCK_H_

#include "types.h" // For AppConfig, AppResources, complex_float_t, etc.
#include <stdbool.h>

/**
 * @brief Initializes the DC block module.
 *
 * This function sets up the necessary liquid-dsp IIR filter object
 * for DC offset removal.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @return true on success, false on failure (e.g., filter creation).
 */
bool dc_block_init(AppConfig* config, AppResources* resources);

/**
 * @brief Applies the DC block filter to a block of samples.
 *
 * This function processes the input samples in-place to remove DC offsets.
 *
 * @param resources Pointer to the application resources (to get the filter object).
 * @param samples Pointer to the complex float samples (modified in-place).
 * @param num_samples The number of complex samples in the block.
 */
void dc_block_apply(AppResources* resources, complex_float_t* samples, int num_samples);

/**
 * @brief Cleans up resources allocated by the DC block module.
 * @param resources Pointer to the application resources.
 */
void dc_block_cleanup(AppResources* resources);

#endif // DC_BLOCK_H_
