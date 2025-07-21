#include "utils.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

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
    static const char* error_msg = "(Error getting size)";
    if (!buffer || buffer_size == 0) return error_msg;

    if (size_bytes < 0) {
        strncpy(buffer, error_msg, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
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
