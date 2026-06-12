/**
 * @file utilities.h
 * @brief Defines the interface for general-purpose utility and helper functions.
 *
 * This module contains a collection of miscellaneous helper functions that are
 * used across various parts of the application. This includes functions for
 * timekeeping, string manipulation, file size formatting, and sample format
 * conversions.
 */

#ifndef UTILITIES_H_
#define UTILITIES_H_

#include <stddef.h>
#include "app_context.h"
#include "module.h"
#include "mem_arena.h"

// --- Function Declarations ---

/**
 * @brief Gets a high-resolution monotonic time in seconds.
 * @return The time in seconds as a double.
 */
double utility_get_time(void);

/**
 * @brief Clears the standard input buffer up to the next newline or EOF.
 */
void utility_clear_stdin(void);

/**
 * @brief Formats a file size in bytes into a human-readable string (B, KB, MB, GB).
 * @param size_bytes The size in bytes.
 * @param buffer A character buffer to store the formatted string.
 * @param buffer_size The size of the character buffer.
 * @return A pointer to the provided buffer containing the formatted string.
 */
const char* utility_format_size(long long size_bytes, char* buffer, size_t buffer_size);

/**
 * @brief Platform-independent helper to get the base filename from a full path.
 * @param config The application configuration, containing the effective path.
 * @param buffer A character buffer to store the resulting basename.
 * @param buffer_size The size of the character buffer.
 * @param arena The memory arena, needed for temporary allocations on POSIX.
 * @return A pointer to the provided buffer containing the basename.
 */
const char* get_basename_for_parsing(const AppConfig *config, char* buffer, size_t buffer_size, MemoryArena* arena);

/**
 * @brief A helper to safely add a new key-value pair to the summary info struct.
 * @param info Pointer to the InputSummaryInfo struct to modify.
 * @param label The label or key for the summary item.
 * @param value_fmt A printf-style format string for the value.
 * @param ... Variable arguments corresponding to the format string.
 */
void add_summary_item(InputSummaryInfo* info, const char* label, const char* value_fmt, ...);

/**
 * @brief Helper function to trim leading/trailing whitespace from a string in-place.
 * @param input_string The string to trim.
 * @return A pointer to the beginning of the trimmed content within the original string.
 */
char* utility_trim_whitespace(char* input_string);

/**
 * @brief Formats a duration in seconds into a human-readable HH:MM:SS string.
 * @param total_seconds The duration in seconds.
 * @param buffer A character buffer to store the formatted string.
 * @param buffer_size The size of the character buffer.
 */
void utility_format_duration(double total_seconds, char* buffer, size_t buffer_size);

/**
 * @brief Checks if a given frequency exceeds the Nyquist frequency for a sample rate and warns the user.
 * @param freq_to_check_hz The frequency in Hz to check.
 * @param sample_rate_hz The sample rate in Hz.
 * @param context_str A string describing the context (e.g., "Filter Cutoff").
 * @return true to continue, false if the user chose to cancel.
 */
bool utility_check_nyquist_warning(double freq_to_check_hz, double sample_rate_hz, const char* context_str);

/**
 * @brief Checks if a file exists at the given path and is accessible for reading.
 * @param full_path The full path to the file.
 * @return true if the file exists and can be opened for reading, false otherwise.
 */
bool utility_check_file_exists(const char* full_path);

#endif // UTILS_H_

/**
 * @brief Prompts the user for permission to overwrite a file.
 */
bool utility_prompt_for_overwrite(const char* path_for_messages);
bool utility_verify_output_path(const struct AppConfig* config, const char* out_path_utf8);
