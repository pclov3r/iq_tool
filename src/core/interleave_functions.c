/**
 * @file interleave_functions.c
 * @brief Functions for interleaving planar I/Q (or stereo L/R) arrays.
 */

#include "interleave_functions.h"
#include <math.h>

// --- Interleaving Helpers Implementation ---

#define DEFINE_IQ_INTERLEAVER(type_name, type)                                 \
  void interleave_##type_name(                                                 \
      const type *restrict i_plane, const type *restrict q_plane,              \
      type *restrict interleaved_out, size_t num_samples) {                    \
    for (size_t sample_index = 0; sample_index < num_samples;                  \
         sample_index++) {                                                     \
      interleaved_out[sample_index * 2] = i_plane[sample_index];               \
      interleaved_out[sample_index * 2 + 1] = q_plane[sample_index];           \
    }                                                                          \
  }

DEFINE_IQ_INTERLEAVER(s8, int8_t)
DEFINE_IQ_INTERLEAVER(u8, uint8_t)
DEFINE_IQ_INTERLEAVER(s16, int16_t)
DEFINE_IQ_INTERLEAVER(u16, uint16_t)

// --- Float to Integer Interleaver ---

// Standard 8/16-bit (Array Indexable)
#define DEFINE_F32_TO_INT_INTERLEAVER(SUFFIX, TYPE, SCALE, OFFSET)             \
  void interleave_f32_to_##SUFFIX(                                             \
      const float *restrict i_plane, const float *restrict q_plane,            \
      TYPE *restrict interleaved_out, size_t num_samples) {                    \
    const float scale_val = (float)(SCALE);                                    \
    const float offset_val = (float)(OFFSET);                                  \
                                                                               \
    for (size_t sample_index = 0; sample_index < num_samples;                  \
         sample_index++) {                                                     \
      float i_sample = i_plane[sample_index];                                  \
      float q_sample = q_plane[sample_index];                                  \
                                                                               \
      i_sample = fminf(fmaxf(i_sample, -1.0f), 1.0f);                          \
      q_sample = fminf(fmaxf(q_sample, -1.0f), 1.0f);                          \
                                                                               \
      i_sample = (i_sample * scale_val) + offset_val;                          \
      q_sample = (q_sample * scale_val) + offset_val;                          \
                                                                               \
      i_sample = (i_sample > 0.0f) ? i_sample + 0.5f : i_sample - 0.5f;        \
      q_sample = (q_sample > 0.0f) ? q_sample + 0.5f : q_sample - 0.5f;        \
                                                                               \
      interleaved_out[sample_index * 2] = (TYPE)i_sample;                      \
      interleaved_out[sample_index * 2 + 1] = (TYPE)q_sample;                  \
    }                                                                          \
  }

// 24-bit (Packed 3-byte)
#define DEFINE_F32_TO_INT24_INTERLEAVER(SUFFIX, SCALE, OFFSET)                 \
  void interleave_f32_to_##SUFFIX(                                             \
      const float *restrict i_plane, const float *restrict q_plane,            \
      uint8_t *restrict interleaved_out, size_t num_samples) {                 \
    const float scale_val = (float)(SCALE);                                    \
    const float offset_val = (float)(OFFSET);                                  \
                                                                               \
    for (size_t sample_index = 0; sample_index < num_samples;                  \
         sample_index++) {                                                     \
      float i_sample = i_plane[sample_index];                                  \
      float q_sample = q_plane[sample_index];                                  \
                                                                               \
      i_sample = fminf(fmaxf(i_sample, -1.0f), 1.0f);                          \
      q_sample = fminf(fmaxf(q_sample, -1.0f), 1.0f);                          \
                                                                               \
      i_sample = (i_sample * scale_val) + offset_val;                          \
      q_sample = (q_sample * scale_val) + offset_val;                          \
                                                                               \
      i_sample = (i_sample > 0.0f) ? i_sample + 0.5f : i_sample - 0.5f;        \
      q_sample = (q_sample > 0.0f) ? q_sample + 0.5f : q_sample - 0.5f;        \
                                                                               \
      int32_t i_int = (int32_t)i_sample;                                       \
      int32_t q_int = (int32_t)q_sample;                                       \
                                                                               \
      interleaved_out[(sample_index * 6) + 0] = (uint8_t)(i_int & 0xFF);       \
      interleaved_out[(sample_index * 6) + 1] =                                \
          (uint8_t)((i_int >> 8) & 0xFF);                                      \
      interleaved_out[(sample_index * 6) + 2] =                                \
          (uint8_t)((i_int >> 16) & 0xFF);                                     \
      interleaved_out[(sample_index * 6) + 3] = (uint8_t)(q_int & 0xFF);       \
      interleaved_out[(sample_index * 6) + 4] =                                \
          (uint8_t)((q_int >> 8) & 0xFF);                                      \
      interleaved_out[(sample_index * 6) + 5] =                                \
          (uint8_t)((q_int >> 16) & 0xFF);                                     \
    }                                                                          \
  }

// 32-bit (Uses double intermediate to prevent float rounding overflow at 2^31-1)
#define DEFINE_F32_TO_INT32_INTERLEAVER(SUFFIX, TYPE, SCALE, OFFSET, MIN_VAL,  \
                                        MAX_VAL)                               \
  void interleave_f32_to_##SUFFIX(                                             \
      const float *restrict i_plane, const float *restrict q_plane,            \
      TYPE *restrict interleaved_out, size_t num_samples) {                    \
    const double scale_val = (double)(SCALE);                                  \
    const double offset_val = (double)(OFFSET);                                \
    const double min_val = (double)(MIN_VAL);                                  \
    const double max_val = (double)(MAX_VAL);                                  \
                                                                               \
    for (size_t sample_index = 0; sample_index < num_samples;                  \
         sample_index++) {                                                     \
      double i_sample = (double)i_plane[sample_index];                         \
      double q_sample = (double)q_plane[sample_index];                         \
                                                                               \
      i_sample = fmin(fmax(i_sample, -1.0), 1.0);                              \
      q_sample = fmin(fmax(q_sample, -1.0), 1.0);                              \
                                                                               \
      i_sample = (i_sample * scale_val) + offset_val;                          \
      q_sample = (q_sample * scale_val) + offset_val;                          \
                                                                               \
      i_sample = (i_sample > 0.0) ? i_sample + 0.5 : i_sample - 0.5;           \
      q_sample = (q_sample > 0.0) ? q_sample + 0.5 : q_sample - 0.5;           \
                                                                               \
      i_sample = fmin(fmax(i_sample, min_val), max_val);                       \
      q_sample = fmin(fmax(q_sample, min_val), max_val);                       \
                                                                               \
      interleaved_out[sample_index * 2] = (TYPE)i_sample;                      \
      interleaved_out[sample_index * 2 + 1] = (TYPE)q_sample;                  \
    }                                                                          \
  }

DEFINE_F32_TO_INT_INTERLEAVER(s8, int8_t, 127.0f, 0.0f)
DEFINE_F32_TO_INT_INTERLEAVER(u8, uint8_t, 127.0f, 128.0f)
DEFINE_F32_TO_INT_INTERLEAVER(s16, int16_t, 32767.0f, 0.0f)
DEFINE_F32_TO_INT_INTERLEAVER(u16, uint16_t, 32767.0f, 32768.0f)
DEFINE_F32_TO_INT24_INTERLEAVER(s24, 8388607.0f, 0.0f)
DEFINE_F32_TO_INT24_INTERLEAVER(u24, 8388607.0f, 8388607.5f)
DEFINE_F32_TO_INT32_INTERLEAVER(s32, int32_t, 2147483647.0, 0.0, -2147483648.0,
                                2147483647.0)
DEFINE_F32_TO_INT32_INTERLEAVER(u32, uint32_t, 2147483647.0, 2147483647.5, 0.0,
                                4294967295.0)
