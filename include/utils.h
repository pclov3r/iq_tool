#ifndef UTILS_H_
#define UTILS_H_

#include <stdint.h>
#include <stddef.h>
#include "types.h" // For AppConfig and SdrSoftwareType

/**
 * @brief Converts a float value to an unsigned char [0, 255].
 *
 * The input float is assumed to be in the range [-127.5, 127.5]. The function
 * shifts this to [0, 255], clamps it to ensure it's within the valid range,
 * and then rounds to the nearest integer.
 *
 * @param v The input float value.
 * @return The corresponding uint8_t value.
 */
uint8_t float_to_uchar(float v);

/**
 * @brief Converts a float value to a signed char [-128, 127].
 *
 * The input float is assumed to be in the range [-128.0, 127.0]. The function
 * clamps it to ensure it's within the valid range and then rounds to the
 * nearest integer.
 *
 * @param v The input float value.
 * @return The corresponding int8_t value.
 */
int8_t float_to_schar(float v);


/**
 * @brief Clears the standard input buffer up to the next newline or EOF.
 *
 * This is a utility function to be called after reading single characters
 * from stdin (e.g., in prompts) to consume any leftover characters,
 * particularly the newline, preventing them from interfering with subsequent
 * input calls.
 */
void clear_stdin_buffer(void);

/**
 * @brief Formats a file size in bytes into a human-readable string (B, KB, MB, GB).
 *
 * Uses base-10 (1000) units for calculation.
 *
 * @param size_bytes The size of the file in bytes. If negative, it's treated as an error.
 * @param buffer A character buffer to write the formatted string into.
 * @param buffer_size The size of the provided buffer.
 * @return A pointer to the provided buffer containing the formatted string, or
 *         a pointer to a static error string if the input size is negative.
 */
const char* format_file_size(long long size_bytes, char* buffer, size_t buffer_size);

/**
 * @brief Platform-independent helper to get the base filename from a path.
 *
 * This is used for parsing metadata from the filename itself. It correctly
 * handles both Windows (wide char) and POSIX paths.
 *
 * @param config The application configuration containing the effective input path.
 * @param buffer A buffer to store the resulting UTF-8 basename.
 * @param buffer_size The size of the buffer.
 * @return A pointer to the buffer on success, or NULL on failure.
 */
const char* get_basename_for_parsing(const AppConfig *config, char* buffer, size_t buffer_size);

/**
 * @brief Converts an SdrSoftwareType enum value to a human-readable string.
 * @param type The enum value to convert.
 * @return A constant string representing the software type.
 */
const char* sdr_software_type_to_string(SdrSoftwareType type);

/**
 * @brief A helper to safely add a new key-value pair to the summary info struct.
 * @param info A pointer to the summary struct to be populated.
 * @param label The label for the summary item.
 * @param value_fmt A printf-style format string for the value.
 * @param ... Additional arguments for the format string.
 */
void add_summary_item(InputSummaryInfo* info, const char* label, const char* value_fmt, ...);

/**
 * @brief Helper function to trim leading/trailing whitespace from a string in-place.
 * @param str The string to trim.
 * @return A pointer to the beginning of the trimmed string.
 */
char* trim_whitespace(char* str);

/**
 * @brief Formats a duration in seconds into a human-readable HH:MM:SS string.
 * @param total_seconds The duration in seconds.
 * @param buffer A character buffer to write the formatted string into.
 * @param buffer_size The size of the provided buffer.
 */
void format_duration(double total_seconds, char* buffer, size_t buffer_size);


#endif // UTILS_H_
