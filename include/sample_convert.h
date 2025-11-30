/**
 * @file sample_convert.h
 * @brief Defines the interface for converting between I/Q sample formats.
 */

#ifndef SAMPLE_CONVERT_H_
#define SAMPLE_CONVERT_H_

#include <stddef.h>
#include <stdint.h>
#include "common_types.h"

// --- Function Declarations ---

/**
 * @brief Gets the number of bytes for a single I/Q pair of the given format.
 * @param format The sample format.
 * @return The size in bytes, or 0 for unknown formats.
 */
size_t get_bytes_per_sample(format_t format);

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
bool convert_block_to_cf32(const void* restrict input_buffer, complex_float_t* restrict output_buffer, size_t num_frames, format_t input_format, float gain);

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
bool convert_cf32_to_block(const complex_float_t* restrict input_buffer, void* restrict output_buffer, size_t num_frames, format_t output_format);

// --- Interleaving Helpers ---
// These functions are used by Input Modules (e.g., SDRplay) that receive Planar data
// (separate I and Q arrays) but must write Interleaved data to the RingBuffer.
// The 'restrict' keyword is crucial here to allow auto-vectorization (SIMD).

/**
 * @brief Interleaves 8-bit signed planar data into an interleaved buffer.
 */
void sample_convert_interleave_s8(const int8_t* restrict i_plane, const int8_t* restrict q_plane, int8_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves 8-bit unsigned planar data into an interleaved buffer.
 */
void sample_convert_interleave_u8(const uint8_t* restrict i_plane, const uint8_t* restrict q_plane, uint8_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves 16-bit signed planar data into an interleaved buffer.
 */
void sample_convert_interleave_s16(const int16_t* restrict i_plane, const int16_t* restrict q_plane, int16_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves 16-bit unsigned planar data into an interleaved buffer.
 */
void sample_convert_interleave_u16(const uint16_t* restrict i_plane, const uint16_t* restrict q_plane, uint16_t* restrict interleaved_out, size_t num_samples);

#endif // SAMPLE_CONVERT_H_
