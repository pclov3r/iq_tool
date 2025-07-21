#ifndef UTILS_H_
#define UTILS_H_

#include <stdint.h> // For uint8_t
#include <stddef.h> // For size_t

// --- Function Declarations ---

/**
 * @brief Converts a float value (assumed centered around 0) to an unsigned char.
 *        Clamps the input range effectively represented by [-127.5, +127.5] to [0, 255].
 * @param v The input float value.
 * @return The corresponding uint8_t value, clamped to [0, 255].
 */
uint8_t float_to_uchar(float v);

/**
 * @brief Clears the standard input buffer up to the next newline or EOF.
 *        Useful after reading single characters (like in prompts) to consume leftovers.
 */
void clear_stdin_buffer(void);

/**
 * @brief Formats a file size in bytes into a human-readable string (B, KB, MB, GB).
 *        Uses base-10 (1000) units.
 * @param size_bytes The size of the file in bytes (long long). If negative, treated as error.
 * @param buffer A character buffer to write the formatted string into.
 * @param buffer_size The size of the provided buffer.
 * @return A pointer to the provided buffer containing the formatted string,
 *         or a pointer to an error string like "(Error getting size)" if input size is negative.
 * @note The function handles negative input size as an error indicator.
 *       Resulting strings will be like "123 B", "45.67 KB", "89.12 MB", "3.45 GB".
 */
const char* format_file_size(long long size_bytes, char* buffer, size_t buffer_size);


#endif // UTILS_H_
