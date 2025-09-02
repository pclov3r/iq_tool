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

// --- Forward Declarations ---
struct AppConfig;
struct AppResources;
// We need complex_float_t for the execute function signature
#include "common_types.h"

// --- Opaque Type Definition ---
// By forward-declaring the struct and using a typedef, we hide the
// implementation (which is liquid-dsp's msresamp_crcf) from any file
// that includes this header.
struct resampler_s;
typedef struct resampler_s resampler_t;

// --- Function Declarations ---

/**
 * @brief Creates and initializes a resampler object.
 */
resampler_t* create_resampler(const struct AppConfig *config, struct AppResources *resources, float resample_ratio);

/**
 * @brief Destroys a resampler object and frees all associated memory.
 */
void destroy_resampler(resampler_t* resampler);

/**
 * @brief Resets the internal state of the resampler object.
 */
void resampler_reset(resampler_t* resampler);

/**
 * @brief Executes the resampler on a block of samples.
 */
void resampler_execute(resampler_t* resampler, complex_float_t* input, unsigned int num_input_frames, complex_float_t* output, unsigned int* num_output_frames);


#endif // RESAMPLER_H_
