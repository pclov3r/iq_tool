/**
 * @file resampler.h
 * @brief Defines the generic interface for the sample rate converter.
 *
 * This module encapsulates the specific implementation of the resampler,
 * hiding the details of the underlying DSP library (e.g., liquid-dsp)
 * from the rest of the application.
 */

#ifndef RESAMPLER_H_
#define RESAMPLER_H_

#include <stdbool.h>
#include <stddef.h> // Added for size_t
#include "common_types.h"

// --- Forward Declarations ---
struct AppConfig;
struct AppContext;

// --- Opaque Type Definition ---
// By forward-declaring the struct and using a typedef, we hide the
// implementation (which is liquid-dsp's msresamp_crcf) from any file
// that includes this header.
struct resampler_s;
typedef struct resampler_s Resampler;

// --- Function Declarations ---

/**
 * @brief Creates and initializes a resampler object.
 */
Resampler* resampler_create(const struct AppConfig *config, struct AppContext* app, float resample_ratio);

/**
 * @brief Destroys a resampler object and frees all associated memory.
 */
void resampler_destroy(Resampler* resampler);

/**
 * @brief Resets the internal state of the resampler object.
 */
void resampler_reset(Resampler* resampler);

/**
 * @brief Executes the resampler on a block of samples.
 *
 * @param resampler The resampler object.
 * @param input Pointer to input buffer.
 * @param num_input_frames Number of frames in input.
 * @param output Pointer to output buffer.
 * @param max_output_capacity The maximum number of samples the output buffer can hold (Guard Rail).
 * @param num_output_frames Pointer to store the actual number of frames produced.
 */
void resampler_execute(Resampler* resampler, ComplexFloat* input, unsigned int num_input_frames, ComplexFloat* output, unsigned int* num_output_frames);

#endif // RESAMPLER_H_
