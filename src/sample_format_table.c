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
    [S8]      = { S8,      "s8",      "s8 (Signed 8-bit Real)",         2,  FORMAT_DBM_OFFSET_8BIT,  FORMAT_ATTENUATION_8BIT  },
    [U8]      = { U8,      "u8",      "u8 (Unsigned 8-bit Real)",       2,  FORMAT_DBM_OFFSET_8BIT,  FORMAT_ATTENUATION_8BIT  },
    [S16]     = { S16,     "s16",     "s16 (Signed 16-bit Real)",       4,  FORMAT_DBM_OFFSET_16BIT, FORMAT_ATTENUATION_16BIT },
    [U16]     = { U16,     "u16",     "u16 (Unsigned 16-bit Real)",     4,  FORMAT_DBM_OFFSET_16BIT, FORMAT_ATTENUATION_16BIT },
    [S32]     = { S32,     "s32",     "s32 (Signed 32-bit Real)",       8,  FORMAT_DBM_OFFSET_32BIT, FORMAT_ATTENUATION_32BIT },
    [U32]     = { U32,     "u32",     "u32 (Unsigned 32-bit Real)",     8,  FORMAT_DBM_OFFSET_32BIT, FORMAT_ATTENUATION_32BIT },
    [F32]     = { F32,     "f32",     "f32 (32-bit Float Real)",        8,  FORMAT_DBM_OFFSET_32BIT, FORMAT_ATTENUATION_32BIT },
    [CU8]     = { CU8,     "cu8",     "cu8 (Unsigned 8-bit Complex)",   2,  FORMAT_DBM_OFFSET_8BIT,  FORMAT_ATTENUATION_8BIT  },
    [CS8]     = { CS8,     "cs8",     "cs8 (Signed 8-bit Complex)",     2,  FORMAT_DBM_OFFSET_8BIT,  FORMAT_ATTENUATION_8BIT  },
    [CU16]    = { CU16,    "cu16",    "cu16 (Unsigned 16-bit Complex)", 4,  FORMAT_DBM_OFFSET_16BIT, FORMAT_ATTENUATION_16BIT },
    [CS16]    = { CS16,    "cs16",    "cs16 (Signed 16-bit Complex)",   4,  FORMAT_DBM_OFFSET_16BIT, FORMAT_ATTENUATION_16BIT },
    [SC16Q11] = { SC16Q11, "sc16q11", "sc16q11 (16-bit Signed Q4.11)",  4,  FORMAT_DBM_OFFSET_16BIT, FORMAT_ATTENUATION_16BIT },
    [CS24]    = { CS24,    "cs24",    "cs24 (Signed 24-bit Complex)",   6,  FORMAT_DBM_OFFSET_24BIT, FORMAT_ATTENUATION_24BIT },
    [CU32]    = { CU32,    "cu32",    "cu32 (Unsigned 32-bit Complex)", 8,  FORMAT_DBM_OFFSET_32BIT, FORMAT_ATTENUATION_32BIT },
    [CS32]    = { CS32,    "cs32",    "cs32 (Signed 32-bit Complex)",   8,  FORMAT_DBM_OFFSET_32BIT, FORMAT_ATTENUATION_32BIT },
    [CF32]    = { CF32,    "cf32",    "cf32 (32-bit Float Complex)",    8,  FORMAT_DBM_OFFSET_32BIT, FORMAT_ATTENUATION_32BIT },
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
