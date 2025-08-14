// utils.c
#include "utils.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdarg.h> // For va_list, va_start, va_end
#include <ctype.h>  // For isspace

#ifdef _WIN32
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#define strcasecmp _stricmp // Add this for Windows compatibility
#else
#include <libgen.h>
#include <strings.h> // For strcasecmp
#endif

/**
 * @brief Converts a float value to an unsigned char [0, 255].
 */
uint8_t float_to_uchar(float v) {
    // Shift the range [-127.5, 127.5] up to [0, 255]
    v += 127.5f;
    // Clamp the value to the valid [0.0, 255.0] range
    v = fmaxf(0.0f, fminf(255.0f, v));
    // Round to the nearest integer and cast
    return (uint8_t)(v + 0.5f);
}

/**
 * @brief Converts a float value to a signed char [-128, 127].
 */
int8_t float_to_schar(float v) {
    // Clamp the value to the valid [-128.0, 127.0] range for int8_t
    v = fmaxf(-128.0f, fminf(127.0f, v));
    // Round to the nearest integer and cast
    return (int8_t)lrintf(v);
}


/**
 * @brief Clears the standard input buffer.
 */
void clear_stdin_buffer(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

/**
 * @brief Formats a file size in bytes into a human-readable string.
 */
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

/**
 * @brief Platform-independent helper to get the base filename from a path.
 */
const char* get_basename_for_parsing(const AppConfig *config, char* buffer, size_t buffer_size) {
#ifdef _WIN32
    if (config->effective_input_filename_w) {
        // On Windows, use the wide-char path and convert the result to UTF-8.
        const wchar_t* base_w = PathFindFileNameW(config->effective_input_filename_w);
        if (WideCharToMultiByte(CP_UTF8, 0, base_w, -1, buffer, buffer_size, NULL, NULL) > 0) {
            return buffer;
        }
    }
#else
    if (config->effective_input_filename) {
        // On POSIX, we need to make a temporary copy because basename() can modify its input.
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

/**
 * @brief Converts an SdrSoftwareType enum value to a human-readable string.
 */
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

/**
 * @brief A helper to safely add a new key-value pair to the summary info struct.
 */
void add_summary_item(InputSummaryInfo* info, const char* label, const char* value_fmt, ...) {
    if (info->count >= MAX_SUMMARY_ITEMS) {
        return; // Prevent buffer overflow
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

/**
 * @brief Helper function to trim leading/trailing whitespace from a string in-place.
 */
char* trim_whitespace(char* str) {
    if (!str) return NULL;
    char* end;

    // Trim leading space
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) { // All spaces?
        return str;
    }

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    // Write new null terminator
    end[1] = '\0';

    return str;
}

/**
 * @brief Formats a duration in seconds into a human-readable HH:MM:SS string.
 */
void format_duration(double total_seconds, char* buffer, size_t buffer_size) {
    if (!isfinite(total_seconds) || total_seconds < 0) {
        snprintf(buffer, buffer_size, "N/A");
        return;
    }
    if (total_seconds > 0 && total_seconds < 1.0) {
        total_seconds = 1.0; // Report at least 1 second for very short runs
    }

    int hours = (int)(total_seconds / 3600);
    total_seconds -= hours * 3600;
    int minutes = (int)(total_seconds / 60);
    total_seconds -= minutes * 60;
    int seconds = (int)round(total_seconds);

    // Handle potential rounding carry-over
    if (seconds >= 60) { minutes++; seconds = 0; }
    if (minutes >= 60) { hours++; minutes = 0; }

    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
}

/**
 * @brief Converts a sample format name string to its corresponding format_t enum.
 *
 * This is the single, centralized implementation for this conversion.
 *
 * @param name The string name of the format (e.g., "cs16").
 * @return The format_t enum value, or FORMAT_UNKNOWN if not found.
 */
format_t utils_get_format_from_string(const char *name) {
    if (strcasecmp(name, "s8") == 0) return S8;
    if (strcasecmp(name, "u8") == 0) return U8;
    if (strcasecmp(name, "s16") == 0) return S16;
    if (strcasecmp(name, "u16") == 0) return U16;
    if (strcasecmp(name, "s32") == 0) return S32;
    if (strcasecmp(name, "u32") == 0) return U32;
    if (strcasecmp(name, "f32") == 0) return F32;
    if (strcasecmp(name, "cs8") == 0) return CS8;
    if (strcasecmp(name, "cu8") == 0) return CU8;
    if (strcasecmp(name, "cs16") == 0) return CS16;
    if (strcasecmp(name, "cu16") == 0) return CU16;
    if (strcasecmp(name, "cs32") == 0) return CS32;
    if (strcasecmp(name, "cu32") == 0) return CU32;
    if (strcasecmp(name, "cf32") == 0) return CF32;
    if (strcasecmp(name, "sc16q11") == 0) return SC16Q11;
    return FORMAT_UNKNOWN;
}
