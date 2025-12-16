/**
 * @file dc_block.h
 * @brief Defines the interface for the DC offset removal (DC block) module.
 *
 * This module provides functionality to initialize, apply, and clean up a
 * high-pass filter designed to remove any DC component from the I/Q signal.
 * This is often a necessary pre-processing step for other DSP algorithms,
 * such as I/Q imbalance correction.
 */

#ifndef DC_BLOCK_H_
#define DC_BLOCK_H_

#include <stdbool.h>
#include "app_context.h" // Provides AppConfig, AppContext, and complex_float_t

// --- Function Declarations ---

/**
 * @brief Creates the DC block filter object.
 *
 * This function sets up the necessary liquid-dsp IIR filter object
 * for DC offset removal if it is enabled in the AppConfig.
 *
 * @param config Pointer to the application configuration.
 * @param app Pointer to the application app where the filter will be stored.
 * @return true on success or if disabled, false on failure (e.g., filter creation).
 */
bool dc_block_create(AppConfig* config, AppContext* app);

/**
 * @brief Resets the internal state of the DC block filter.
 *
 * This should be called upon a stream discontinuity to clear the filter's
 * internal state, preventing old data from corrupting new samples.
 *
 * @param app Pointer to the application app containing the filter object.
 */
void dc_block_reset(DspContext* dsp);

/**
 * @brief Applies the DC block filter to a block of samples.
 *
 * This function processes the input samples in-place to remove DC offsets.
 * If the module is disabled or not initialized, it does nothing.
 *
 * @param app Pointer to the application app (to get the filter object).
 * @param samples Pointer to the complex float samples (modified in-place).
 * @param num_samples The number of complex samples in the block.
 */
void dc_block_apply(DspContext* dsp, complex_float_t* samples, int num_samples);

/**
 * @brief Cleans up app allocated by the DC block module.
 *
 * This function destroys the liquid-dsp filter object if it was created.
 *
 * @param app Pointer to the application app.
 */
void dc_block_destroy(AppContext* app);

#endif // DC_BLOCK_H_
