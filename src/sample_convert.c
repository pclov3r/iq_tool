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
#include "log.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
 * @brief Converts a block of raw input samples into normalized, gain-adjusted complex floats.
 */
bool convert_raw_to_cf32(const void* input_buffer, complex_float_t* output_buffer, size_t num_frames, format_t input_format, float gain) {
    size_t i;

    switch (input_format) {
        case CS8: {
            const int8_t* in = (const int8_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_norm = (float)in[i * 2] / 128.0f;
                float q_norm = (float)in[i * 2 + 1] / 128.0f;
                output_buffer[i] = (i_norm * gain) + I * (q_norm * gain);
            }
            break;
        }
        case CU8: {
            const uint8_t* in = (const uint8_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_norm = ((float)in[i * 2] - 127.5f) / 128.0f;
                float q_norm = ((float)in[i * 2 + 1] - 127.5f) / 128.0f;
                output_buffer[i] = (i_norm * gain) + I * (q_norm * gain);
            }
            break;
        }
        case CS16: {
            const int16_t* in = (const int16_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_norm = (float)in[i * 2] / 32768.0f;
                float q_norm = (float)in[i * 2 + 1] / 32768.0f;
                output_buffer[i] = (i_norm * gain) + I * (q_norm * gain);
            }
            break;
        }
        case SC16Q11: {
            const int16_t* in = (const int16_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_norm = (float)in[i * 2] / 2048.0f;
                float q_norm = (float)in[i * 2 + 1] / 2048.0f;
                output_buffer[i] = (i_norm * gain) + I * (q_norm * gain);
            }
            break;
        }
        case CU16: {
            const uint16_t* in = (const uint16_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_norm = ((float)in[i * 2] - 32767.5f) / 32768.0f;
                float q_norm = ((float)in[i * 2 + 1] - 32767.5f) / 32768.0f;
                output_buffer[i] = (i_norm * gain) + I * (q_norm * gain);
            }
            break;
        }
        case CS32: {
            const int32_t* in = (const int32_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                double i_norm = (double)in[i * 2] / 2147483648.0;
                double q_norm = (double)in[i * 2 + 1] / 2147483648.0;
                output_buffer[i] = (float)(i_norm * gain) + I * (float)(q_norm * gain);
            }
            break;
        }
        case CU32: {
            const uint32_t* in = (const uint32_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                double i_norm = ((double)in[i * 2] - 2147483647.5) / 2147483648.0;
                double q_norm = ((double)in[i * 2 + 1] - 2147483647.5) / 2147483648.0;
                output_buffer[i] = (float)(i_norm * gain) + I * (float)(q_norm * gain);
            }
            break;
        }
        case CF32: {
            const complex_float_t* in = (const complex_float_t*)input_buffer;
            for (i = 0; i < num_frames; ++i) {
                output_buffer[i] = in[i] * gain;
            }
            break;
        }
        default:
            log_error("Unhandled input format in convert_raw_to_cf32: %d", input_format);
            return false;
    }
    return true;
}

/**
 * @brief Converts a block of normalized complex floats into the specified output byte format.
 */
bool convert_cf32_to_block(const complex_float_t* input_buffer, void* output_buffer, size_t num_frames, format_t output_format) {
    size_t i;

    switch (output_format) {
        case CS8: {
            int8_t* out = (int8_t*)output_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_val = crealf(input_buffer[i]) * 127.0f;
                float q_val = cimagf(input_buffer[i]) * 127.0f;
                out[i * 2]     = (int8_t)fmaxf(-128.0f, fminf(127.0f, lrintf(i_val)));
                out[i * 2 + 1] = (int8_t)fmaxf(-128.0f, fminf(127.0f, lrintf(q_val)));
            }
            break;
        }
        case CU8: {
            uint8_t* out = (uint8_t*)output_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_val = (crealf(input_buffer[i]) * 127.0f) + 127.5f;
                float q_val = (cimagf(input_buffer[i]) * 127.0f) + 127.5f;
                out[i * 2]     = (uint8_t)fmaxf(0.0f, fminf(255.0f, lrintf(i_val)));
                out[i * 2 + 1] = (uint8_t)fmaxf(0.0f, fminf(255.0f, lrintf(q_val)));
            }
            break;
        }
        case CS16: {
            int16_t* out = (int16_t*)output_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_val = crealf(input_buffer[i]) * 32767.0f;
                float q_val = cimagf(input_buffer[i]) * 32767.0f;
                out[i * 2]     = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, lrintf(i_val)));
                out[i * 2 + 1] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, lrintf(q_val)));
            }
            break;
        }
        case SC16Q11: {
            int16_t* out = (int16_t*)output_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_val = crealf(input_buffer[i]) * 2048.0f;
                float q_val = cimagf(input_buffer[i]) * 2048.0f;
                out[i * 2]     = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, lrintf(i_val)));
                out[i * 2 + 1] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, lrintf(q_val)));
            }
            break;
        }
        case CU16: {
            uint16_t* out = (uint16_t*)output_buffer;
            for (i = 0; i < num_frames; ++i) {
                float i_val = (crealf(input_buffer[i]) * 32767.0f) + 32767.5f;
                float q_val = (cimagf(input_buffer[i]) * 32767.0f) + 32767.5f;
                out[i * 2]     = (uint16_t)fmaxf(0.0f, fminf(65535.0f, lrintf(i_val)));
                out[i * 2 + 1] = (uint16_t)fmaxf(0.0f, fminf(65535.0f, lrintf(q_val)));
            }
            break;
        }
        case CS32: {
            int32_t* out = (int32_t*)output_buffer;
            for (i = 0; i < num_frames; ++i) {
                double i_val = creal(input_buffer[i]) * 2147483647.0;
                double q_val = cimag(input_buffer[i]) * 2147483647.0;
                out[i * 2]     = (int32_t)fmax(-2147483648.0, fmin(2147483647.0, llrint(i_val)));
                out[i * 2 + 1] = (int32_t)fmax(-2147483648.0, fmin(2147483647.0, llrint(q_val)));
            }
            break;
        }
        case CU32: {
            uint32_t* out = (uint32_t*)output_buffer;
            for (i = 0; i < num_frames; ++i) {
                double i_val = (creal(input_buffer[i]) * 2147483647.0) + 2147483647.5;
                double q_val = (cimag(input_buffer[i]) * 2147483647.0) + 2147483647.5;
                out[i * 2]     = (uint32_t)fmax(0.0, fmin(4294967295.0, llrint(i_val)));
                out[i * 2 + 1] = (uint32_t)fmax(0.0, fmin(4294967295.0, llrint(q_val)));
            }
            break;
        }
        case CF32: {
            memcpy(output_buffer, input_buffer, num_frames * sizeof(complex_float_t));
            break;
        }
        default:
            log_error("Unhandled output format in convert_cf32_to_block: %d", output_format);
            return false;
    }
    return true;
}
