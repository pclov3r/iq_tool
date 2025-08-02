// sample_convert.c

#include "sample_convert.h"
#include "config.h"
#include "log.h"
#include "types.h"
#include <complex.h>
#include <stdint.h>

#define UNUSED(x) (void)(x)

// --- Forward Declarations for Standard Worker Functions ---
static bool convert_s16_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_u16_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_s8_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_u8_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);

// --- ADDED: Forward Declarations for NEW Native 8-bit Worker Functions ---
static bool convert_s8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);
static bool convert_u8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);


// --- Public Functions ---

// MODIFIED: Signature changed to accept AppConfig so we can check the new flag.
void setup_sample_converter(AppConfig *config, AppResources *resources) {
    switch (resources->input_pcm_format) {
        case PCM_FORMAT_S16:
            resources->converter = convert_s16_to_complex;
            break;
        case PCM_FORMAT_U16:
            resources->converter = convert_u16_to_complex;
            break;
        case PCM_FORMAT_S8:
            // MODIFIED: Select the correct worker based on the user's choice.
            resources->converter = config->native_8bit_path ? convert_s8_native_to_complex : convert_s8_to_complex;
            break;
        case PCM_FORMAT_U8:
            // MODIFIED: Select the correct worker based on the user's choice.
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
    return resources->converter(config, resources, item);
}

// --- Standard Worker Implementations (Unchanged) ---

static bool convert_s16_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    UNUSED(resources);
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
    UNUSED(resources);
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
    UNUSED(resources);
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
    UNUSED(resources);
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

// --- ADDED: New Native 8-bit Worker Implementations ---

static bool convert_s8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    UNUSED(resources);
    const int8_t* restrict raw_buffer = (const int8_t*)item->raw_input_buffer;
    const float scale = config->scale_value;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        // NO SCALE_8_TO_16 multiplication
        const float i_float = (float)raw_buffer[base_idx];
        const float q_float = (float)raw_buffer[base_idx + 1];
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}

static bool convert_u8_native_to_complex(AppConfig *config, AppResources *resources, WorkItem *item) {
    UNUSED(resources);
    const uint8_t* restrict raw_buffer = (const uint8_t*)item->raw_input_buffer;
    const float scale = config->scale_value;
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
        const size_t base_idx = 2 * (size_t)i;
        // NO SCALE_8_TO_16 multiplication

        const float i_float = (float)raw_buffer[base_idx] - 127.5f;
        const float q_float = (float)raw_buffer[base_idx + 1] - 127.5f;
        item->complex_buffer_scaled[i] = (i_float * scale) + I * (q_float * scale);
    }
    return true;
}
