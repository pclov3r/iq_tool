/**
 * @file sample_conversion_functions.h
 * @brief Defines the interface for converting between I/Q sample formats.
 */

#ifndef SAMPLE_CONVERSION_FUNCTIONS_H_
#define SAMPLE_CONVERSION_FUNCTIONS_H_

#include <stddef.h>
#include <stdint.h>
#include "common_types.h"

// --- Function Declarations ---

/**
 * @brief Gets the number of bytes for a single I/Q pair of the given format.
 * @param format The sample format.
 * @return The size in bytes, or 0 for unknown formats.
 */

/**
 * @brief Converts a block of samples from a source format to complex float (cf32).
 *
 * This function handles all supported integer and float input formats, normalizes
 * them to the standard [-1.0, 1.0] range, and applies the specified linear gain
 * multiplier in a single pass.
 *
 * @param input_buffer Pointer to the source data block. Marked 'const' as it's read-only.
 * @param output_buffer Pointer to the destination buffer for complex float data.
 * @param num_frames The number of frames (I/Q pairs) to convert.
 * @param input_format The format of the source data.
 * @param gain The linear gain multiplier to apply.
 * @return true on success, false if the input format is unhandled.
 */
bool sample_convert_block_to_cf32(const void* restrict input_buffer, ComplexFloat* restrict output_buffer, size_t num_frames, SampleFormat input_format, float gain);

/**
 * @brief Converts a block of complex float (cf32) samples to a target output format.
 *
 * This function takes the pipeline's internal complex float data and converts it
 * to the final integer-based format for output, performing the necessary scaling
 * and clamping.
 *
 * @param input_buffer Pointer to the source complex float data. Marked 'const' as it's read-only.
 * @param output_buffer Pointer to the destination buffer for the output data block.
 * @param num_frames The number of frames (I/Q pairs) to convert.
 * @param output_format The target format for the output data.
 * @return true on success, false if the output format is unhandled.
 */
bool sample_convert_cf32_to_block(const ComplexFloat* restrict input_buffer, void* restrict output_buffer, size_t num_frames, SampleFormat output_format);

#endif // SAMPLE_CONVERSION_FUNCTIONS_H_
