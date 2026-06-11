/**
 * @file interleave_functions.h
 * @brief Functions for interleaving left/right planes or I/Q arrays into packed blocks.
 */

#ifndef INTERLEAVE_FUNCTIONS_H_
#define INTERLEAVE_FUNCTIONS_H_

#include <stddef.h>
#include <stdint.h>

// --- Interleaving Helpers (Input: Integer -> Integer) ---
void interleave_s8(const int8_t* restrict i_plane, const int8_t* restrict q_plane, int8_t* restrict interleaved_out, size_t num_samples);
void interleave_u8(const uint8_t* restrict i_plane, const uint8_t* restrict q_plane, uint8_t* restrict interleaved_out, size_t num_samples);
void interleave_s16(const int16_t* restrict i_plane, const int16_t* restrict q_plane, int16_t* restrict interleaved_out, size_t num_samples);
void interleave_u16(const uint16_t* restrict i_plane, const uint16_t* restrict q_plane, uint16_t* restrict interleaved_out, size_t num_samples);

// --- Float to Integer Interleavers (Output: Float -> Integer) ---
void interleave_f32_to_s8(const float* restrict left_plane, const float* restrict right_plane, int8_t* restrict interleaved_out, size_t num_samples);
void interleave_f32_to_u8(const float* restrict left_plane, const float* restrict right_plane, uint8_t* restrict interleaved_out, size_t num_samples);
void interleave_f32_to_s16(const float* restrict left_plane, const float* restrict right_plane, int16_t* restrict interleaved_out, size_t num_samples);
void interleave_f32_to_u16(const float* restrict left_plane, const float* restrict right_plane, uint16_t* restrict interleaved_out, size_t num_samples);
void interleave_f32_to_s24(const float* restrict left_plane, const float* restrict right_plane, uint8_t* restrict interleaved_out, size_t num_samples);
void interleave_f32_to_u24(const float* restrict left_plane, const float* restrict right_plane, uint8_t* restrict interleaved_out, size_t num_samples);
void interleave_f32_to_s32(const float* restrict left_plane, const float* restrict right_plane, int32_t* restrict interleaved_out, size_t num_samples);
void interleave_f32_to_u32(const float* restrict left_plane, const float* restrict right_plane, uint32_t* restrict interleaved_out, size_t num_samples);

#endif // INTERLEAVE_FUNCTIONS_H_
