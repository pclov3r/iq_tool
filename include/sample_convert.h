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
size_t sample_convert_bytes_per_sample(SampleFormat format);

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

// --- Interleaving Helpers (Input: Integer -> Integer) ---
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


// --- Float to Integer Interleavers (Output: Float -> Integer) ---
// These functions are used by DSP modules (e.g., WFM) to pack internal floating-point
// buffers into integer output formats. They perform hard clamping to [-1.0, 1.0]
// before scaling to the target integer range.

/**
 * @brief Interleaves Planar F32 to Interleaved Signed 8-bit (S8).
 * Scales input by 127.0.
 */
void sample_convert_interleave_f32_to_s8(const float* restrict left_plane, const float* restrict right_plane, int8_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves Planar F32 to Interleaved Unsigned 8-bit (U8).
 * Scales input by 127.0 and adds 128.0 offset.
 */
void sample_convert_interleave_f32_to_u8(const float* restrict left_plane, const float* restrict right_plane, uint8_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves Planar F32 to Interleaved Signed 16-bit (S16).
 * Scales input by 32767.0.
 */
void sample_convert_interleave_f32_to_s16(const float* restrict left_plane, const float* restrict right_plane, int16_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves Planar F32 to Interleaved Unsigned 16-bit (U16).
 * Scales input by 32767.0 and adds 32768.0 offset.
 */
void sample_convert_interleave_f32_to_u16(const float* restrict left_plane, const float* restrict right_plane, uint16_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves Planar F32 to Interleaved Signed 24-bit (S24).
 * Packs data into 3-byte chunks. Scales input by 8388607.0.
 * @param interleaved_out Pointer to the destination buffer (treated as byte array).
 */
void sample_convert_interleave_f32_to_s24(const float* restrict left_plane, const float* restrict right_plane, uint8_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves Planar F32 to Interleaved Unsigned 24-bit (U24).
 * Packs data into 3-byte chunks. Scales input by 8388607.0 and adds offset.
 * @param interleaved_out Pointer to the destination buffer (treated as byte array).
 */
void sample_convert_interleave_f32_to_u24(const float* restrict left_plane, const float* restrict right_plane, uint8_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves Planar F32 to Interleaved Signed 32-bit (S32).
 * Scales input by 2147483647.0.
 */
void sample_convert_interleave_f32_to_s32(const float* restrict left_plane, const float* restrict right_plane, int32_t* restrict interleaved_out, size_t num_samples);

/**
 * @brief Interleaves Planar F32 to Interleaved Unsigned 32-bit (U32).
 * Scales input by 2147483647.0 and adds 2147483648.0 offset.
 */
void sample_convert_interleave_f32_to_u32(const float* restrict left_plane, const float* restrict right_plane, uint32_t* restrict interleaved_out, size_t num_samples);

#endif // SAMPLE_CONVERT_H_
