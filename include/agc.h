/**
 * @file agc.h
 * @brief Defines the interface for the Output Automatic Gain Control module.
 *
 * This module provides functionality to initialize, apply, and clean up an
 * automatic gain control (AGC) object. It supports three distinct profiles:
 * 1. DX: Slow RMS tracking for weak/fading signals.
 * 2. Local: Fast RMS tracking for strong analog signals.
 * 3. Digital: Peak-Detect & Lock for digital signals.
 */

#ifndef AGC_H_
#define AGC_H_

#include <stdbool.h>
#include "app_context.h" // Provides AppConfig, AppResources, and complex_float_t

// --- Function Declarations ---

/**
 * @brief Creates and configures the AGC logic based on the application configuration.
 *
 * Depending on the selected profile, this will either create a liquid-dsp
 * AGC object (for DX/Local) or initialize the internal state for the custom
 * Peak-Lock logic (for Digital).
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @return true on success or if disabled, false on failure.
 */
bool agc_create(AppConfig* config, AppResources* resources);

/**
 * @brief Applies the AGC to a block of complex samples.
 *
 * This function processes the input samples in-place.
 * - For DX/Local: It uses the liquid-dsp feedback loop.
 * - For Digital: It scans for peaks and applies provisional gain (if unlocking) 
 *   or applies a static gain (if locked).
 *
 * @param resources Pointer to the application resources.
 * @param samples Pointer to the complex float samples (modified in-place).
 * @param num_samples The number of complex samples in the block.
 */
void agc_apply(AppResources* resources, complex_float_t* samples, unsigned int num_samples);

/**
 * @brief Resets the internal state of the AGC.
 *
 * Resets gain to unity, clears peak memory, and resets locking timers.
 *
 * @param resources Pointer to the application resources.
 */
void agc_reset(AppResources* resources);

/**
 * @brief Cleans up resources allocated by the AGC module.
 *
 * @param resources Pointer to the application resources.
 */
void agc_destroy(AppResources* resources);

#endif // AGC_H_
