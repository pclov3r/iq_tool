#ifndef IQ_CORRECT_H_
#define IQ_CORRECT_H_

#include "types.h" // <-- MODIFIED: Include types for MemoryArena definition
#include <stdbool.h>

/**
 * @brief Initializes the I/Q correction module.
 *
 * This function sets up the necessary liquid-dsp FFT plan and allocates
 * buffers required for the I/Q imbalance optimization algorithm from the
 * provided memory arena.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @param arena The memory arena to use for all buffer allocations.
 * @return true on success, false on failure.
 */
bool iq_correct_init(AppConfig* config, AppResources* resources, MemoryArena* arena);

/**
 * @brief Applies the current I/Q imbalance correction to a block of samples.
 *
 * @param resources Pointer to the application resources.
 * @param samples Pointer to the complex float samples (modified in-place).
 * @param num_samples The number of complex samples in the block.
 */
void iq_correct_apply(AppResources* resources, complex_float_t* samples, int num_samples);

/**
 * @brief Runs the I/Q imbalance optimization algorithm on a block of samples.
 *
 * @param resources Pointer to the application resources.
 * @param optimization_data Pointer to the block of complex float samples to analyze.
 */
void iq_correct_run_optimization(AppResources* resources, const complex_float_t* optimization_data);

/**
 * @brief Cleans up resources allocated by the I/Q correction module.
 * @param resources Pointer to the application resources.
 */
void iq_correct_cleanup(AppResources* resources);


#endif // IQ_CORRECT_H_
