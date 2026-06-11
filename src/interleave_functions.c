/**
 * @file interleave_functions.c
 * @brief Functions for interleaving left/right planes or I/Q arrays.
 */

#include "interleave_functions.h"
#include <math.h>

// --- Interleaving Helpers Implementation ---

#define DEFINE_IQ_INTERLEAVER(type_name, type) \
void interleave_##type_name( \
    const type* restrict i_plane, \
    const type* restrict q_plane, \
    type* restrict interleaved_out, \
    size_t num_samples) \
{ \
    for (size_t k = 0; k < num_samples; k++) { \
        interleaved_out[k * 2]     = i_plane[k]; \
        interleaved_out[k * 2 + 1] = q_plane[k]; \
    } \
}

DEFINE_IQ_INTERLEAVER(s8, int8_t)
DEFINE_IQ_INTERLEAVER(u8, uint8_t)
DEFINE_IQ_INTERLEAVER(s16, int16_t)
DEFINE_IQ_INTERLEAVER(u16, uint16_t)

// --- Float to Integer Interleaver ---

// Standard 8/16/32-bit (Array Indexable)
#define DEFINE_F32_TO_INT_INTERLEAVER(SUFFIX, TYPE, SCALE, OFFSET) \
void interleave_f32_to_##SUFFIX( \
    const float* restrict left_plane, \
    const float* restrict right_plane, \
    TYPE* restrict interleaved_out, \
    size_t num_samples) \
{ \
    const float scale_val = (float)(SCALE); \
    const float offset_val = (float)(OFFSET); \
    \
    for (size_t i = 0; i < num_samples; i++) { \
        float l = left_plane[i]; \
        float r = right_plane[i]; \
        \
        l = fminf(fmaxf(l, -1.0f), 1.0f); \
        r = fminf(fmaxf(r, -1.0f), 1.0f); \
        \
        l = (l * scale_val) + offset_val; \
        r = (r * scale_val) + offset_val; \
        \
        l = (l > 0.0f) ? l + 0.5f : l - 0.5f; \
        r = (r > 0.0f) ? r + 0.5f : r - 0.5f; \
        \
        interleaved_out[i * 2]     = (TYPE)l; \
        interleaved_out[i * 2 + 1] = (TYPE)r; \
    } \
}

// 24-bit (Packed 3-byte)
#define DEFINE_F32_TO_INT24_INTERLEAVER(SUFFIX, SCALE, OFFSET) \
void interleave_f32_to_##SUFFIX( \
    const float* restrict left_plane, \
    const float* restrict right_plane, \
    uint8_t* restrict interleaved_out, \
    size_t num_samples) \
{ \
    const float scale_val = (float)(SCALE); \
    const float offset_val = (float)(OFFSET); \
    \
    for (size_t i = 0; i < num_samples; i++) { \
        float l = left_plane[i]; \
        float r = right_plane[i]; \
        \
        l = fminf(fmaxf(l, -1.0f), 1.0f); \
        r = fminf(fmaxf(r, -1.0f), 1.0f); \
        \
        l = (l * scale_val) + offset_val; \
        r = (r * scale_val) + offset_val; \
        \
        l = (l > 0.0f) ? l + 0.5f : l - 0.5f; \
        r = (r > 0.0f) ? r + 0.5f : r - 0.5f; \
        \
        int32_t il = (int32_t)l; \
        int32_t ir = (int32_t)r; \
        \
        interleaved_out[(i * 6) + 0] = (uint8_t)(il & 0xFF); \
        interleaved_out[(i * 6) + 1] = (uint8_t)((il >> 8) & 0xFF); \
        interleaved_out[(i * 6) + 2] = (uint8_t)((il >> 16) & 0xFF); \
        interleaved_out[(i * 6) + 3] = (uint8_t)(ir & 0xFF); \
        interleaved_out[(i * 6) + 4] = (uint8_t)((ir >> 8) & 0xFF); \
        interleaved_out[(i * 6) + 5] = (uint8_t)((ir >> 16) & 0xFF); \
    } \
}

DEFINE_F32_TO_INT_INTERLEAVER(s8, int8_t, 127.0f, 0.0f)
DEFINE_F32_TO_INT_INTERLEAVER(u8, uint8_t, 127.0f, 128.0f)
DEFINE_F32_TO_INT_INTERLEAVER(s16, int16_t, 32767.0f, 0.0f)
DEFINE_F32_TO_INT_INTERLEAVER(u16, uint16_t, 32767.0f, 32768.0f)
DEFINE_F32_TO_INT24_INTERLEAVER(s24, 8388607.0f, 0.0f)
DEFINE_F32_TO_INT24_INTERLEAVER(u24, 8388607.0f, 8388607.5f)
DEFINE_F32_TO_INT_INTERLEAVER(s32, int32_t, 2147483647.0f, 0.0f)
DEFINE_F32_TO_INT_INTERLEAVER(u32, uint32_t, 2147483647.0f, 2147483648.0f)
