/*
 * sample_convert.c: Functions for converting between various I/Q sample formats.
 *
 * This file is part of iq_tool.
 *
 * The conversion logic in this file is derived from the 'convert-samples'
 * project by Guillaume LE VAILLANT. The original source can be found at:
 * https://codeberg.org/glv/convert-samples
 *
 * Copyright (C) 2021-2022 Guillaume LE VAILLANT
 * Copyright (C) 2025 iq_tool
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "sample_convert.h"
#include "common_types.h" // Provides format_t, complex_float_t
#include "log.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#include <assert.h> // Added for robust error checking in debug builds

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CF32_TO_BLOCK_SIGNED(TYPE, TYPE_MAX_CONST, TYPE_MIN_CONST, SCALE) \
    do { \
        TYPE* out = (TYPE*)output_buffer; \
        const float max_val = (float)TYPE_MAX_CONST; \
        const float min_val = (float)TYPE_MIN_CONST; \
        for (size_t i = 0; i < num_frames; ++i) { \
            float i_val = crealf(input_buffer[i]) * (SCALE); \
            float q_val = cimagf(input_buffer[i]) * (SCALE); \
            i_val = (i_val > 0.0f) ? i_val + 0.5f : i_val - 0.5f; \
            q_val = (q_val > 0.0f) ? q_val + 0.5f : q_val - 0.5f; \
            if (i_val > max_val) i_val = max_val; \
            if (i_val < min_val) i_val = min_val; \
            if (q_val > max_val) q_val = max_val; \
            if (q_val < min_val) q_val = min_val; \
            out[i * 2]     = (TYPE)i_val; \
            out[i * 2 + 1] = (TYPE)q_val; \
        } \
    } while (0)

#define CF32_TO_BLOCK_UNSIGNED(TYPE, TYPE_MAX_CONST, SCALE, OFFSET) \
    do { \
        TYPE* out = (TYPE*)output_buffer; \
        const float max_val = (float)TYPE_MAX_CONST; \
        for (size_t i = 0; i < num_frames; ++i) { \
            float i_val = (crealf(input_buffer[i]) * (SCALE)) + (OFFSET); \
            float q_val = (cimagf(input_buffer[i]) * (SCALE)) + (OFFSET); \
            if (i_val > max_val) i_val = max_val; \
            if (i_val < 0.0f)    i_val = 0.0f; \
            if (q_val > max_val) q_val = max_val; \
            if (q_val < 0.0f)    q_val = 0.0f; \
            out[i * 2]     = (TYPE)(i_val + 0.5f); \
            out[i * 2 + 1] = (TYPE)(q_val + 0.5f); \
        } \
    } while (0)

#define BLOCK_TO_CF32_SIGNED(TYPE, NORMALIZER) \
    do { \
        const TYPE* in = (const TYPE*)input_buffer; \
        const float normalizer_val = (NORMALIZER); \
        for (size_t i = 0; i < num_frames; ++i) { \
            float i_norm = (float)in[i * 2] * normalizer_val; \
            float q_norm = (float)in[i * 2 + 1] * normalizer_val; \
            output_buffer[i] = (i_norm * gain) + I * (q_norm * gain); \
        } \
    } while (0)

#define BLOCK_TO_CF32_UNSIGNED(TYPE, OFFSET, NORMALIZER) \
    do { \
        const TYPE* in = (const TYPE*)input_buffer; \
        const float offset_val = (OFFSET); \
        const float normalizer_val = (NORMALIZER); \
        for (size_t i = 0; i < num_frames; ++i) { \
            float i_norm = ((float)in[i * 2] - offset_val) * normalizer_val; \
            float q_norm = ((float)in[i * 2 + 1] - offset_val) * normalizer_val; \
            output_buffer[i] = (i_norm * gain) + I * (q_norm * gain); \
        } \
    } while (0)


/**
 * @brief Gets the number of bytes for a single sample of the given format.
 */
size_t get_bytes_per_sample(format_t format) {
    switch (format) {
        case S8:   return sizeof(int8_t);
        case U8:   return sizeof(uint8_t);
        case S16:  return sizeof(int16_t);
        case U16:  return sizeof(uint16_t);
        case S32:  return sizeof(int32_t);
        case U32:  return sizeof(uint32_t);
        case F32:  return sizeof(float);
        case CS8:  return sizeof(int8_t) * 2;
        case CU8:  return sizeof(uint8_t) * 2;
        case CS16: return sizeof(int16_t) * 2;
        case CU16: return sizeof(uint16_t) * 2;
        case CS24: return 3 * 2;
        case CS32: return sizeof(int32_t) * 2;
        case CU32: return sizeof(uint32_t) * 2;
        case CF32: return sizeof(complex_float_t);
        case SC16Q11: return sizeof(int16_t) * 2;
        default:   return 0;
    }
}

/**
 * @brief Converts a block of samples from a source format to complex float (cf32).
 */
bool convert_block_to_cf32(const void* restrict input_buffer, complex_float_t* restrict output_buffer, size_t num_frames, format_t input_format, float gain) {
    // Add robustness checks for debug builds. These compile to nothing in release builds.
    assert(input_buffer != NULL && "Input buffer cannot be null.");
    assert(output_buffer != NULL && "Output buffer cannot be null.");

    // The switch statement is placed OUTSIDE the main loop. This is critical.
    // It allows the compiler to select the correct, simple inner loop at the
    // start, enabling effective auto-vectorization.
    switch (input_format) {
        case CS8:
            // Normalize by 128.0 to map [-128, 127] to [-1.0, ~0.992]
            BLOCK_TO_CF32_SIGNED(int8_t, 1.0f / 128.0f);
            break;
        case CU8:
            // Offset by 127.5 (midpoint of [0,255]) to center the range on zero.
            BLOCK_TO_CF32_UNSIGNED(uint8_t, 127.5f, 1.0f / 128.0f);
            break;
        case CS16:
            BLOCK_TO_CF32_SIGNED(int16_t, 1.0f / 32768.0f);
            break;
        case SC16Q11:
            // For Q4.11 format, the value is stored with 11 fractional bits.
            // To convert to float, we divide by 2^11.
            BLOCK_TO_CF32_SIGNED(int16_t, 1.0f / 2048.0f);
            break;
        case CS24: {
            const uint8_t* restrict in_ptr = (const uint8_t*)input_buffer;
            float* restrict out_raw = (float*)output_buffer;
            // 1.0 / 2^23
            const float norm_factor = (1.0f / 8388608.0f) * gain;

            size_t i = 0;

            // Reads 4 bytes, sign-extends 24-bit to 32-bit, normalizes, and stores.
            // Uses 'do-while(0)' to ensure it behaves like a single statement.
            // We read 4 bytes to get the 3 bytes we need because 32-bit loads are 
            // faster/safer than 24-bit loads on most CPUs.
            #define CS24_IN_STEP(out_idx, byte_offset) \
                do { \
                    int32_t val; \
                    memcpy(&val, in_ptr + (byte_offset), 4); \
                    val = (val << 8) >> 8; \
                    out_raw[(i * 2) + (out_idx)] = (float)val * norm_factor; \
                } while (0)

            // Unrolled Loop: Processes 4 samples (8 components) per iteration.
            // This exposes instruction-level parallelism to the CPU.
            for (; i + 4 <= num_frames; i += 4) {
                CS24_IN_STEP(0, 0);  // Sample 0 I
                CS24_IN_STEP(1, 3);  // Sample 0 Q
                CS24_IN_STEP(2, 6);  // Sample 1 I
                CS24_IN_STEP(3, 9);  // Sample 1 Q
                CS24_IN_STEP(4, 12); // Sample 2 I
                CS24_IN_STEP(5, 15); // Sample 2 Q
                CS24_IN_STEP(6, 18); // Sample 3 I
                CS24_IN_STEP(7, 21); // Sample 3 Q
                in_ptr += 24;
            }

            // Tail Loop: Processes remaining samples one by one.
            for (; i < num_frames; ++i) {
                CS24_IN_STEP(0, 0); // I
                CS24_IN_STEP(1, 3); // Q
                in_ptr += 6;
            }

            #undef CS24_IN_STEP
            break;
        }
        case CU16:
            BLOCK_TO_CF32_UNSIGNED(uint16_t, 32767.5f, 1.0f / 32768.0f);
            break;
        case CS32: {
            // This case is handled separately to maintain double precision during normalization.
            const int32_t* in = (const int32_t*)input_buffer;
            // Use double for intermediate precision to avoid losing info from the 32-bit int.
            const double normalizer = 1.0 / 2147483648.0;
            for (size_t i = 0; i < num_frames; ++i) {
                double i_norm = (double)in[i * 2] * normalizer;
                double q_norm = (double)in[i * 2 + 1] * normalizer;
                output_buffer[i] = (float)(i_norm * gain) + I * (float)(q_norm * gain);
            }
            break;
        }
        case CU32: {
            // This case is handled separately to maintain double precision during normalization.
            const uint32_t* in = (const uint32_t*)input_buffer;
            const double offset = 2147483647.5; // (UINT_MAX + 1) / 2.0
            const double normalizer = 1.0 / 2147483648.0;
            for (size_t i = 0; i < num_frames; ++i) {
                double i_norm = ((double)in[i * 2] - offset) * normalizer;
                double q_norm = ((double)in[i * 2 + 1] - offset) * normalizer;
                output_buffer[i] = (float)(i_norm * gain) + I * (float)(q_norm * gain);
            }
            break;
        }
        case CF32: {
            // This case is a direct copy and gain multiplication, no normalization needed.
            const complex_float_t* in = (const complex_float_t*)input_buffer;
            for (size_t i = 0; i < num_frames; ++i) {
                output_buffer[i] = in[i] * gain;
            }
            break;
        }
        default:
            log_error("Unhandled input format: %d", input_format);
            return false;
    }
    return true;
}

/**
 * @brief Converts a block of complex float (cf32) samples to a target output format.
 */
bool convert_cf32_to_block(const complex_float_t* restrict input_buffer, void* restrict output_buffer, size_t num_frames, format_t output_format) {
    // Add robustness checks for debug builds.
    assert(input_buffer != NULL && "Input buffer cannot be null.");
    assert(output_buffer != NULL && "Output buffer cannot be null.");

    switch (output_format) {
        case CS8:
            CF32_TO_BLOCK_SIGNED(int8_t, SCHAR_MAX, SCHAR_MIN, (float)SCHAR_MAX);
            break;
        case CU8:
            CF32_TO_BLOCK_UNSIGNED(uint8_t, UCHAR_MAX, 127.0f, 127.5f);
            break;
        case CS16:
            CF32_TO_BLOCK_SIGNED(int16_t, SHRT_MAX, SHRT_MIN, (float)SHRT_MAX);
            break;
        case SC16Q11:
            CF32_TO_BLOCK_SIGNED(int16_t, SHRT_MAX, SHRT_MIN, 2048.0f);
            break;
        case CU16:
            CF32_TO_BLOCK_UNSIGNED(uint16_t, USHRT_MAX, 32767.0f, 32767.5f);
            break;
        case CS24: {
            const float* restrict in_raw = (const float*)input_buffer;
            uint8_t* restrict out_ptr = (uint8_t*)output_buffer;

            const float scale = 8388607.0f;     // 2^23 - 1
            const float max_val = 8388607.0f;
            const float min_val = -8388608.0f;  // -2^23

            size_t i = 0;

            // Scales, rounds, clamps, and packs float into 3 bytes (24-bit int).
            // OPTIMIZATION: Writes 4 bytes (int32) using memcpy instead of 3 byte writes.
            // The 4th byte is "garbage" that gets overwritten by the next sample's write.
            // This reduces 3 memory writes to 1 unaligned store.
            #define CS24_OUT_STEP(in_idx, byte_offset) \
                do { \
                    float f = in_raw[(i * 2) + (in_idx)] * scale; \
                    f = (f > 0.0f) ? f + 0.5f : f - 0.5f; \
                    if (f > max_val) f = max_val; \
                    else if (f < min_val) f = min_val; \
                    int32_t v = (int32_t)f; \
                    memcpy(out_ptr + (byte_offset), &v, 4); \
                } while (0)

            // Unrolled Loop: Processes 4 samples (8 components) per iteration.
            for (; i + 4 <= num_frames; i += 4) {
                CS24_OUT_STEP(0, 0);
                CS24_OUT_STEP(1, 3);
                CS24_OUT_STEP(2, 6);
                CS24_OUT_STEP(3, 9);
                CS24_OUT_STEP(4, 12);
                CS24_OUT_STEP(5, 15);
                CS24_OUT_STEP(6, 18);
                CS24_OUT_STEP(7, 21);
                out_ptr += 24;
            }

            // Tail Loop: Processes remaining samples one by one.
            for (; i < num_frames; ++i) {
                CS24_OUT_STEP(0, 0); // I
                CS24_OUT_STEP(1, 3); // Q
                out_ptr += 6;
            }

            #undef CS24_OUT_STEP
            break;
        }
        case CS32: {
            const float* restrict in_raw = (const float*)input_buffer;
            int32_t* restrict out_ptr = (int32_t*)output_buffer;

            // Use double precision to avoid float rounding overflow at INT_MAX
            const double scale = 2147483647.0;
            const double max_val = 2147483647.0;
            const double min_val = -2147483648.0;

            size_t i = 0;

            // Converts one float component to int32 via double precision
            #define CS32_OUT_STEP(idx) \
                do { \
                    double d = (double)in_raw[i*2 + (idx)] * scale; \
                    d = (d > 0.0) ? d + 0.5 : d - 0.5; \
                    if (d > max_val) d = max_val; \
                    else if (d < min_val) d = min_val; \
                    out_ptr[i*2 + (idx)] = (int32_t)d; \
                } while (0)

            // Unrolled Loop: Processes 2 samples (4 components) per iteration.
            // 4 Doubles = 256 bits (1 AVX2 Register).
            for (; i + 2 <= num_frames; i += 2) {
                CS32_OUT_STEP(0);
                CS32_OUT_STEP(1);
                CS32_OUT_STEP(2);
                CS32_OUT_STEP(3);
            }

            // Tail loop
            for (; i < num_frames; ++i) {
                CS32_OUT_STEP(0); // I
                CS32_OUT_STEP(1); // Q
            }

            #undef CS32_OUT_STEP
            break;
        }
        case CU32: {
            const float* restrict in_raw = (const float*)input_buffer;
            uint32_t* restrict out_ptr = (uint32_t*)output_buffer;

            const double scale = 2147483647.0;
            const double offset = 2147483647.5;
            const double max_val = 4294967295.0; // UINT_MAX

            size_t i = 0;

            // Converts one float component to uint32 via double precision
            #define CU32_OUT_STEP(idx) \
                do { \
                    double d = ((double)in_raw[i*2 + (idx)] * scale) + offset; \
                    d += 0.5; \
                    if (d > max_val) d = max_val; \
                    else if (d < 0.0) d = 0.0; \
                    out_ptr[i*2 + (idx)] = (uint32_t)d; \
                } while (0)

            // Unrolled Loop: Processes 2 samples (4 components) per iteration.
            for (; i + 2 <= num_frames; i += 2) {
                CU32_OUT_STEP(0);
                CU32_OUT_STEP(1);
                CU32_OUT_STEP(2);
                CU32_OUT_STEP(3);
            }

            // Tail loop
            for (; i < num_frames; ++i) {
                CU32_OUT_STEP(0); // I
                CU32_OUT_STEP(1); // Q
            }

            #undef CU32_OUT_STEP
            break;
        }
        case CF32:
            memcpy(output_buffer, input_buffer, num_frames * sizeof(complex_float_t));
            break;
        default:
            log_error("Unhandled output format: %d", output_format);
            return false;
    }
    return true;
}

// --- Interleaving Helpers Implementation ---

void sample_convert_interleave_s8(const int8_t* restrict i_plane, const int8_t* restrict q_plane, int8_t* restrict interleaved_out, size_t num_samples) {
    for (size_t k = 0; k < num_samples; k++) {
        interleaved_out[k * 2]     = i_plane[k];
        interleaved_out[k * 2 + 1] = q_plane[k];
    }
}

void sample_convert_interleave_u8(const uint8_t* restrict i_plane, const uint8_t* restrict q_plane, uint8_t* restrict interleaved_out, size_t num_samples) {
    for (size_t k = 0; k < num_samples; k++) {
        interleaved_out[k * 2]     = i_plane[k];
        interleaved_out[k * 2 + 1] = q_plane[k];
    }
}

void sample_convert_interleave_s16(const int16_t* restrict i_plane, const int16_t* restrict q_plane, int16_t* restrict interleaved_out, size_t num_samples) {
    for (size_t k = 0; k < num_samples; k++) {
        interleaved_out[k * 2]     = i_plane[k];
        interleaved_out[k * 2 + 1] = q_plane[k];
    }
}

void sample_convert_interleave_u16(const uint16_t* restrict i_plane, const uint16_t* restrict q_plane, uint16_t* restrict interleaved_out, size_t num_samples) {
    for (size_t k = 0; k < num_samples; k++) {
        interleaved_out[k * 2]     = i_plane[k];
        interleaved_out[k * 2 + 1] = q_plane[k];
    }
}
