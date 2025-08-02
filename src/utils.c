// utils.c
#include "utils.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#ifdef _WIN32
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#else
#include <libgen.h>
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
