/**
 * @file utils.h
 * @brief Defines the interface for general-purpose utility and helper functions.
 *
 * This module contains a collection of miscellaneous helper functions that are
 * used across various parts of the application. This includes functions for
 * timekeeping, string manipulation, file size formatting, and sample format
 * conversions.
 */

#ifndef UTILS_H_
#define UTILS_H_

#include <stdint.h>
#include <stddef.h>
#include "app_context.h"  // Provides AppConfig
#include "input_source.h" // Provides InputSummaryInfo, SdrSoftwareType
#include "memory_arena.h" // Provides MemoryArena

// --- Function Declarations ---

/**
 * @brief Gets a high-resolution monotonic time in seconds.
 * @return The time in seconds as a double.
 */
double get_monotonic_time_sec(void);

/**
 * @brief Clears the standard input buffer up to the next newline or EOF.
 */
void clear_stdin_buffer(void);

/**
 * @brief Formats a file size in bytes into a human-readable string (B, KB, MB, GB).
 * @param size_bytes The size in bytes.
 * @param buffer A character buffer to store the formatted string.
 * @param buffer_size The size of the character buffer.
 * @return A pointer to the provided buffer containing the formatted string.
 */
const char* format_file_size(long long size_bytes, char* buffer, size_t buffer_size);

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
 * @brief Converts an SdrSoftwareType enum value to a human-readable string.
 * @param type The enum value.
 * @return A constant string representing the software name.
 */
const char* sdr_software_type_to_string(SdrSoftwareType type);

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
 * @param str The string to trim.
 * @return A pointer to the beginning of the trimmed content within the original string.
 */
char* trim_whitespace(char* str);

/**
 * @brief Formats a duration in seconds into a human-readable HH:MM:SS string.
 * @param total_seconds The duration in seconds.
 * @param buffer A character buffer to store the formatted string.
 * @param buffer_size The size of the character buffer.
 */
void format_duration(double total_seconds, char* buffer, size_t buffer_size);

/**
 * @brief Converts a sample format name string (e.g., "cs16") to its corresponding format_t enum.
 * @param name The string name of the format.
 * @return The format_t enum value, or FORMAT_UNKNOWN if not found.
 */
format_t utils_get_format_from_string(const char *name);

/**
 * @brief Converts a format_t enum value to its full, human-readable description.
 * @param format The enum value.
 * @return A constant string with the full description.
 */
const char* utils_get_format_description_string(format_t format);

/**
 * @brief Checks if a given frequency exceeds the Nyquist frequency for a sample rate and warns the user.
 * @param freq_to_check_hz The frequency in Hz to check.
 * @param sample_rate_hz The sample rate in Hz.
 * @param context_str A string describing the context (e.g., "Filter Cutoff").
 * @return true to continue, false if the user chose to cancel.
 */
bool utils_check_nyquist_warning(double freq_to_check_hz, double sample_rate_hz, const char* context_str);

/**
 * @brief Checks if a file exists at the given path and is accessible for reading.
 * @param full_path The full path to the file.
 * @return true if the file exists and can be opened for reading, false otherwise.
 */
bool utils_check_file_exists(const char* full_path);

#endif // UTILS_H_
