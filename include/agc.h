/**
 * @file agc.h
 * @brief Defines the interface for the Output Automatic Gain Control module.
 *
 * This module provides functionality to initialize, apply, and clean up an
 * automatic gain control (AGC) object. It utilizes a self-contained Harris/LMS block-level algorithm.
 */

#ifndef AGC_H_
#define AGC_H_

#include <stdbool.h>
#include "app_context.h" // Provides AppConfig, AppContext, and ComplexFloat
#include "argparse.h"    // Provides struct argparse_option

// --- Function Declarations ---

/**
 * @brief Creates and configures the AGC logic based on the application configuration.
 *
 * Initializes the internal state for the block-level gain tracking loop.
 *
 * @param config Pointer to the application configuration.
 * @param app Pointer to the application app.
 * @return true on success or if disabled, false on failure.
 */
bool agc_create(AppConfig* config, AppContext* app);

/**
 * @brief Applies the AGC to a block of complex samples.
 *
 * This function processes the input samples in-place.
 * Measures block RMS, computes error, applies deadband, and updates gain via LMS.
 *
 * @param app Pointer to the application app.
 * @param samples Pointer to the complex float samples (modified in-place).
 * @param num_samples The number of complex samples in the block.
 */
void agc_apply(DspContext* dsp, ComplexFloat* samples, unsigned int num_samples);

/**
 * @brief Resets the internal state of the AGC.
 *
 * Resets gain to unity, clears peak memory, and resets locking timers.
 *
 * @param app Pointer to the application app.
 */
void agc_reset(DspContext* dsp);

/**
 * @brief Cleans up app allocated by the AGC module.
 *
 * @param app Pointer to the application app.
 */
void agc_destroy(AppContext* app);

#endif // AGC_H_
