/*
 * sample_convert.c: Functions for converting between various I/Q sample formats.
 *
 * This file is part of iq_resample_tool.
 *
 * The conversion logic in this file is derived from the 'convert-samples'
 * project by Guillaume LE VAILLANT. The original source can be found at:
 * https://codeberg.org/glv/convert-samples
 *
 * Copyright (C) 2021-2022 Guillaume LE VAILLANT
 * Copyright (C) 2025 iq_resample_tool
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
        case CS32:
            // This case is handled separately from the macro because it requires
            // double precision for intermediate calculations to avoid data loss.
            do {
                int32_t* out = (int32_t*)output_buffer;
                const double max_val = (double)INT_MAX;
                const double min_val = (double)INT_MIN;
                for (size_t i = 0; i < num_frames; ++i) {
                    double i_val = creal(input_buffer[i]) * max_val;
                    double q_val = cimag(input_buffer[i]) * max_val;
                    i_val = (i_val > 0.0) ? i_val + 0.5 : i_val - 0.5;
                    q_val = (q_val > 0.0) ? q_val + 0.5 : q_val - 0.5;
                    if (i_val > max_val) i_val = max_val;
                    if (i_val < min_val) i_val = min_val;
                    if (q_val > max_val) q_val = max_val;
                    if (q_val < min_val) q_val = min_val;
                    out[i * 2]     = (int32_t)i_val;
                    out[i * 2 + 1] = (int32_t)q_val;
                }
            } while (0);
            break;
        case CU32:
            // This case also requires double precision.
            do {
                uint32_t* out = (uint32_t*)output_buffer;
                const double max_val = (double)UINT_MAX;
                for (size_t i = 0; i < num_frames; ++i) {
                    double i_val = (creal(input_buffer[i]) * 2147483647.0) + 2147483647.5;
                    double q_val = (cimag(input_buffer[i]) * 2147483647.0) + 2147483647.5;
                    if (i_val > max_val) i_val = max_val;
                    if (i_val < 0.0)     i_val = 0.0;
                    if (q_val > max_val) q_val = max_val;
                    if (q_val < 0.0)     q_val = 0.0;
                    out[i * 2]     = (uint32_t)(i_val + 0.5);
                    out[i * 2 + 1] = (uint32_t)(q_val + 0.5);
                }
            } while(0);
            break;
        case CF32:
            memcpy(output_buffer, input_buffer, num_frames * sizeof(complex_float_t));
            break;
        default:
            log_error("Unhandled output format: %d", output_format);
            return false;
    }
    return true;
}
