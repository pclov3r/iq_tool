// sample_convert.c

#include "sample_convert.h"
#include "config.h"
#include "log.h"
#include "types.h"
#include "utils.h"      // Required for float_to_uchar and float_to_schar
#include <complex.h>
#include <stdint.h>
#include <math.h>       // Required for fmaxf, fminf, and lrintf

// --- Forward Declarations for Input Conversion Worker Functions ---
static bool convert_s16_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_u16_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_s8_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_u8_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_s8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_u8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);


// --- Public Functions ---

void setup_sample_converter(AppConfig *config, AppResources *resources) {
    switch (resources->input_pcm_format) {
        case PCM_FORMAT_S16:
            resources->converter = convert_s16_to_complex;
            break;
        case PCM_FORMAT_U16:
            resources->converter = convert_u16_to_complex;
            break;
        case PCM_FORMAT_S8:
            resources->converter = config->native_8bit_path ? convert_s8_native_to_complex : convert_s8_to_complex;
            break;
        case PCM_FORMAT_U8:
            resources->converter = config->native_8bit_path ? convert_u8_native_to_complex : convert_u8_to_complex;
            break;
        default:
            log_fatal("Internal Error: Cannot select a sample converter for unhandled format %d.",
                      (int)resources->input_pcm_format);
            resources->converter = NULL;
            break;
    }
}

bool convert_raw_input_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    if (!resources->converter) return false;
    return resources->converter(config, resources, item);
}

void convert_complex_to_output_format(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int num_frames) {
    (void)resources; // This parameter is currently unused, but kept for future flexibility.
    if (num_frames == 0) return;

    // Determine which complex buffer to use as the final source
    complex_float_t *final_complex_data = (config->shift_after_resample && resources->shifter_nco != NULL)
                                               ? item->complex_buffer_shifted
                                               : item->complex_buffer_resampled;

    if (config->sample_format == SAMPLE_TYPE_CU8) {
        uint8_t *out_ptr_u8 = (uint8_t*)item->output_buffer;
        for (unsigned int i = 0; i < num_frames; ++i) {
            *out_ptr_u8++ = float_to_uchar(crealf(final_complex_data[i]));
            *out_ptr_u8++ = float_to_uchar(cimagf(final_complex_data[i]));
        }
    } else if (config->sample_format == SAMPLE_TYPE_CS8) {
        int8_t *out_ptr_s8 = (int8_t*)item->output_buffer;
        for (unsigned int i = 0; i < num_frames; ++i) {
            *out_ptr_s8++ = float_to_schar(crealf(final_complex_data[i]));
            *out_ptr_s8++ = float_to_schar(cimagf(final_complex_data[i]));
        }
    } else { // Handles SAMPLE_TYPE_CS16
        int16_t *out_ptr_s16 = (int16_t*)item->output_buffer;
        for (unsigned int i = 0; i < num_frames; ++i) {
            float real_val = crealf(final_complex_data[i]);
            float imag_val = cimagf(final_complex_data[i]);
            // Clamp the values to the valid 16-bit signed integer range before casting
            real_val = fmaxf(-32768.0f, fminf(32767.0f, real_val));
            imag_val = fmaxf(-32768.0f, fminf(32767.0f, imag_val));
            *out_ptr_s16++ = (int16_t)lrintf(real_val);
            *out_ptr_s16++ = (int16_t)lrintf(imag_val);
        }
    }
}


// --- Input Conversion Worker Implementations ---

static bool convert_s16_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    (void)resources;
    const int16_t* restrict raw_buffer = (const int16_t*)item->raw_input_buffer;
    const float scale = config->scale_value;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        const float i_float = (float)raw_buffer[base_idx];
        const float q_float = (float)raw_buffer[base_idx + 1];
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}

static bool convert_u16_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    (void)resources;
    const uint16_t* restrict raw_buffer = (const uint16_t*)item->raw_input_buffer;
    const float scale = config->scale_value;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        const float i_float = (float)raw_buffer[base_idx] - 32767.5f;
        const float q_float = (float)raw_buffer[base_idx + 1] - 32767.5f;
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}

static bool convert_s8_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    (void)resources;
    const int8_t* restrict raw_buffer = (const int8_t*)item->raw_input_buffer;
    const float scale = config->scale_value * SCALE_8_TO_16;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        const float i_float = (float)raw_buffer[base_idx];
        const float q_float = (float)raw_buffer[base_idx + 1];
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}

static bool convert_u8_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    (void)resources;
    const uint8_t* restrict raw_buffer = (const uint8_t*)item->raw_input_buffer;
    const float scale = config->scale_value * SCALE_8_TO_16;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        const float i_float = (float)raw_buffer[base_idx] - 127.5f;
        const float q_float = (float)raw_buffer[base_idx + 1] - 127.5f;
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}

static bool convert_s8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    (void)resources;
    const int8_t* restrict raw_buffer = (const int8_t*)item->raw_input_buffer;
    const float scale = config->scale_value;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        const float i_float = (float)raw_buffer[base_idx];
        const float q_float = (float)raw_buffer[base_idx + 1];
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}

static bool convert_u8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    (void)resources;
    const uint8_t* restrict raw_buffer = (const uint8_t*)item->raw_input_buffer;
    const float scale = config->scale_value;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        const float i_float = (float)raw_buffer[base_idx] - 127.5f;
        const float q_float = (float)raw_buffer[base_idx + 1] - 127.5f;
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}
