/**
 * @file sample_format_table.c
 */

#include "sample_format_table.h"
#include "constants.h"
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

// --- The Master Table ---
static const SampleFormatInfo MASTER_FORMAT_TABLE[] = {
    [S8]      = { S8,      "s8",      "s8 (Signed 8-bit Real)",         1, DEFAULT_FILTER_ATTENUATION_8BIT_DB  },
    [U8]      = { U8,      "u8",      "u8 (Unsigned 8-bit Real)",       1, DEFAULT_FILTER_ATTENUATION_8BIT_DB  },
    [S16]     = { S16,     "s16",     "s16 (Signed 16-bit Real)",       2, DEFAULT_FILTER_ATTENUATION_16BIT_DB },
    [U16]     = { U16,     "u16",     "u16 (Unsigned 16-bit Real)",     2, DEFAULT_FILTER_ATTENUATION_16BIT_DB },
    [S24]     = { S24,     "s24",     "s24 (Signed 24-bit Real)",       3, DEFAULT_FILTER_ATTENUATION_24BIT_DB },
    [U24]     = { U24,     "u24",     "u24 (Unsigned 24-bit Real)",     3, DEFAULT_FILTER_ATTENUATION_24BIT_DB },
    [S32]     = { S32,     "s32",     "s32 (Signed 32-bit Real)",       4, DEFAULT_FILTER_ATTENUATION_32BIT_DB },
    [U32]     = { U32,     "u32",     "u32 (Unsigned 32-bit Real)",     4, DEFAULT_FILTER_ATTENUATION_32BIT_DB },
    [F32]     = { F32,     "f32",     "f32 (32-bit Float Real)",        4, DEFAULT_FILTER_ATTENUATION_32BIT_DB },
    [CU8]     = { CU8,     "cu8",     "cu8 (Unsigned 8-bit Complex)",   2, DEFAULT_FILTER_ATTENUATION_8BIT_DB  },
    [CS8]     = { CS8,     "cs8",     "cs8 (Signed 8-bit Complex)",     2, DEFAULT_FILTER_ATTENUATION_8BIT_DB  },
    [CU16]    = { CU16,    "cu16",    "cu16 (Unsigned 16-bit Complex)", 4, DEFAULT_FILTER_ATTENUATION_16BIT_DB },
    [CS16]    = { CS16,    "cs16",    "cs16 (Signed 16-bit Complex)",   4, DEFAULT_FILTER_ATTENUATION_16BIT_DB },
    [SC16Q11] = { SC16Q11, "sc16q11", "sc16q11 (16-bit Signed Q4.11)",  4, DEFAULT_FILTER_ATTENUATION_16BIT_DB },
    [CS24]    = { CS24,    "cs24",    "cs24 (Signed 24-bit Complex)",   6, DEFAULT_FILTER_ATTENUATION_24BIT_DB },
    [CU32]    = { CU32,    "cu32",    "cu32 (Unsigned 32-bit Complex)", 8, DEFAULT_FILTER_ATTENUATION_32BIT_DB },
    [CS32]    = { CS32,    "cs32",    "cs32 (Signed 32-bit Complex)",   8, DEFAULT_FILTER_ATTENUATION_32BIT_DB },
    [CF32]    = { CF32,    "cf32",    "cf32 (32-bit Float Complex)",    8, DEFAULT_FILTER_ATTENUATION_32BIT_DB },
};

static const int NUM_MASTER_FORMATS = sizeof(MASTER_FORMAT_TABLE) / sizeof(MASTER_FORMAT_TABLE[0]);

// --- Function Implementations ---
const SampleFormatInfo* get_format_info_by_enum(SampleFormat format) {
    if (format > FORMAT_UNKNOWN && format < NUM_MASTER_FORMATS) {
        if (MASTER_FORMAT_TABLE[format].name_str != NULL) {
            return &MASTER_FORMAT_TABLE[format];
        }
    }
    return NULL;
}

const SampleFormatInfo* get_format_info_by_name(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < NUM_MASTER_FORMATS; ++i) {
        if (MASTER_FORMAT_TABLE[i].name_str && strcasecmp(name, MASTER_FORMAT_TABLE[i].name_str) == 0) {
            return &MASTER_FORMAT_TABLE[i];
        }
    }
    return NULL;
}

size_t get_bytes_per_iq_sample(SampleFormat format) {
    const SampleFormatInfo* info = get_format_info_by_enum(format);
    return info ? info->bytes_per_iq_sample : 0;
}
