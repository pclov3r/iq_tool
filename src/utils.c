// utils.c
#include "utils.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#ifdef _WIN32
#include <shlwapi.h>
#define strcasecmp _stricmp
#else
#include <libgen.h>
#include <strings.h>
#endif

// --- The Single Source of Truth for Sample Formats ---
typedef struct {
    format_t format_enum;
    const char* name_str;
    const char* description_str;
} SampleFormatInfo;

static const SampleFormatInfo format_table[] = {
    { S8,      "s8",      "s8 (Signed 8-bit Real)" },
    { U8,      "u8",      "u8 (Unsigned 8-bit Real)" },
    { S16,     "s16",     "s16 (Signed 16-bit Real)" },
    { U16,     "u16",     "u16 (Unsigned 16-bit Real)" },
    { S32,     "s32",     "s32 (Signed 32-bit Real)" },
    { U32,     "u32",     "u32 (Unsigned 32-bit Real)" },
    { F32,     "f32",     "f32 (32-bit Float Real)" },
    { CU8,     "cu8",     "cu8 (Unsigned 8-bit Complex)" },
    { CS8,     "cs8",     "cs8 (Signed 8-bit Complex)" },
    { CU16,    "cu16",    "cu16 (Unsigned 16-bit Complex)" },
    { CS16,    "cs16",    "cs16 (Signed 16-bit Complex)" },
    { CU32,    "cu32",    "cu32 (Unsigned 32-bit Complex)" },
    { CS32,    "cs32",    "cs32 (Signed 32-bit Complex)" },
    { CF32,    "cf32",    "cf32 (32-bit Float Complex)" },
    { SC16Q11, "sc16q11", "sc16q11 (16-bit Signed Complex Q4.11)" },
};
static const int num_formats = sizeof(format_table) / sizeof(format_table[0]);

// MODIFIED: The definitions for float_to_uchar and float_to_schar are REMOVED from this file.

// ... (the rest of the file, from clear_stdin_buffer onwards, is unchanged) ...

void clear_stdin_buffer(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

const char* format_file_size(long long size_bytes, char* buffer, size_t buffer_size) {
    static const char* error_msg = "(N/A)";
    if (!buffer || buffer_size == 0) return error_msg;
    if (size_bytes < 0) {
        snprintf(buffer, buffer_size, "%s", error_msg);
        return buffer;
    }
    double size_d = (double)size_bytes;
    const long long kilo = 1000;
    const long long mega = 1000 * 1000;
    const long long giga = 1000 * 1000 * 1000;
    if (size_bytes < kilo) {
        snprintf(buffer, buffer_size, "%lld B", size_bytes);
    } else if (size_bytes < mega) {
        snprintf(buffer, buffer_size, "%.2f KB", size_d / kilo);
    } else if (size_bytes < giga) {
        snprintf(buffer, buffer_size, "%.2f MB", size_d / mega);
    } else {
        snprintf(buffer, buffer_size, "%.2f GB", size_d / giga);
    }
    return buffer;
}

const char* get_basename_for_parsing(const AppConfig *config, char* buffer, size_t buffer_size) {
#ifdef _WIN32
    if (config->effective_input_filename_w) {
        const wchar_t* base_w = PathFindFileNameW(config->effective_input_filename_w);
        if (WideCharToMultiByte(CP_UTF8, 0, base_w, -1, buffer, buffer_size, NULL, NULL) > 0) {
            return buffer;
        }
    }
#else
    if (config->effective_input_filename) {
        char* temp_copy = strdup(config->effective_input_filename);
        if (temp_copy) {
            char* base = basename(temp_copy);
            strncpy(buffer, base, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            free(temp_copy);
            return buffer;
        }
    }
#endif
    return NULL;
}

const char* sdr_software_type_to_string(SdrSoftwareType type) {
    switch (type) {
        case SDR_SOFTWARE_UNKNOWN: return "Unknown";
        case SDR_CONSOLE:          return "SDR Console";
        case SDR_SHARP:            return "SDR#";
        case SDR_UNO:              return "SDRuno";
        case SDR_CONNECT:          return "SDRconnect";
        default:                   return "Invalid Type";
    }
}

void add_summary_item(InputSummaryInfo* info, const char* label, const char* value_fmt, ...) {
    if (info->count >= MAX_SUMMARY_ITEMS) {
        return;
    }
    SummaryItem* item = &info->items[info->count];
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->label[sizeof(item->label) - 1] = '\0';
    va_list args;
    va_start(args, value_fmt);
    vsnprintf(item->value, sizeof(item->value), value_fmt, args);
    va_end(args);
    item->value[sizeof(item->value) - 1] = '\0';
    info->count++;
}

char* trim_whitespace(char* str) {
    if (!str) return NULL;
    char* end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) {
        return str;
    }
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

void format_duration(double total_seconds, char* buffer, size_t buffer_size) {
    if (!isfinite(total_seconds) || total_seconds < 0) {
        snprintf(buffer, buffer_size, "N/A");
        return;
    }
    if (total_seconds > 0 && total_seconds < 1.0) {
        total_seconds = 1.0;
    }
    int hours = (int)(total_seconds / 3600);
    total_seconds -= hours * 3600;
    int minutes = (int)(total_seconds / 60);
    total_seconds -= minutes * 60;
    int seconds = (int)round(total_seconds);
    if (seconds >= 60) { minutes++; seconds = 0; }
    if (minutes >= 60) { hours++; minutes = 0; }
    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
}

format_t utils_get_format_from_string(const char *name) {
    if (!name) return FORMAT_UNKNOWN;
    for (int i = 0; i < num_formats; ++i) {
        if (strcasecmp(name, format_table[i].name_str) == 0) {
            return format_table[i].format_enum;
        }
    }
    return FORMAT_UNKNOWN;
}

const char* utils_get_format_description_string(format_t format) {
    for (int i = 0; i < num_formats; ++i) {
        if (format == format_table[i].format_enum) {
            return format_table[i].description_str;
        }
    }
    return "Unknown";
}
