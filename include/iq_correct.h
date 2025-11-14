/**
 * @file iq_correct.h
 * @brief Defines the interface for the automatic I/Q imbalance correction module.
 *
 * This module implements an algorithm to detect and correct for I/Q imbalance
 * (gain and phase errors) in the signal. It works by analyzing the signal's
 * spectrum to measure the asymmetry between the positive and negative frequencies
 * and then iteratively adjusts correction factors to minimize this asymmetry.
 */

#ifndef IQ_CORRECT_H_
#define IQ_CORRECT_H_

#include <stdbool.h>
#include "app_context.h"
#include "memory_arena.h"
#include <sndfile.h> // Needed for the SNDFILE* type in the function signature

// --- Function Declarations ---

/**
 * @brief Initializes the I/Q correction module.
 *
 * This function sets up the necessary liquid-dsp FFT plan and allocates
 * all required workspace buffers from the provided memory arena.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources where correction state will be stored.
 * @param arena The memory arena to use for all buffer allocations.
 * @return true on success or if disabled, false on failure.
 */
bool iq_correct_init(AppConfig* config, AppResources* resources, MemoryArena* arena);

/**
 * @brief Applies the current I/Q imbalance correction to a block of samples.
 *
 * This function reads the latest correction factors (in a thread-safe manner)
 * and applies them to the provided sample buffer in-place.
 *
 * @param resources Pointer to the application resources.
 * @param samples Pointer to the complex float samples (modified in-place).
 * @param num_samples The number of complex samples in the block.
 */
void iq_correct_apply(AppResources* resources, complex_float_t* samples, int num_samples);

/**
 * @brief Runs one pass of the I/Q imbalance optimization algorithm.
 *
 * This function analyzes the provided block of samples to measure the current
 * imbalance and updates the global correction factors. This is typically run
 * in a separate, lower-priority thread.
 *
 * @param resources Pointer to the application resources.
 * @param optimization_data Pointer to the block of complex float samples to analyze.
 */
void iq_correct_run_optimization(AppResources* resources, const complex_float_t* optimization_data);

/**
 * @brief Cleans up resources allocated by the I/Q correction module.
 *
 * This function destroys the liquid-dsp FFT plan object.
 *
 * @param resources Pointer to the application resources.
 */
void iq_correct_destroy(AppResources* resources);

/**
 * @brief Performs a synchronous, one-shot I/Q calibration pass for file-based inputs.
 * This should be called by file-based input modules during their pre-stream phase.
 * It reads from the file, runs the optimization, and rewinds the file.
 *
 * @param ctx The application context.
 * @param infile The handle to the open input file (e.g., from libsndfile).
 * @return true on success, false on a critical failure.
 */
bool iq_correct_run_initial_calibration(ModuleContext* ctx, SNDFILE* infile);


#endif // IQ_CORRECT_H_
