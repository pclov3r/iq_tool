#ifndef PLATFORM_H_
#define PLATFORM_H_

#include <stdbool.h> // Needed for bool return types
#include <stddef.h>  // Needed for size_t

// Include platform-specific headers needed for type definitions in function signatures
#ifdef _WIN32
#include <windows.h> // For DWORD
#endif

// --- Function Declarations ---

#ifdef _WIN32

/**
 * @brief Sets stdout to binary mode on Windows.
 * @return true on success, false on failure.
 */
bool set_stdout_binary(void);

/**
 * @brief Converts a path argument (ANSI/MBCS) to absolute Wide and UTF-8 paths.
 *        This version writes directly into pre-allocated buffers, avoiding heap allocation.
 * @param path_arg_mbcs The input path string (system's default ANSI code page).
 * @param out_path_w Pointer to the destination buffer for the wide character absolute path.
 * @param out_path_w_size The size of the out_path_w buffer in characters.
 * @param out_path_utf8 Pointer to the destination buffer for the UTF-8 encoded absolute path.
 * @param out_path_utf8_size The size of the out_path_utf8 buffer in bytes.
 * @return true on success, false on failure (e.g., buffer too small, API errors).
 */
bool get_absolute_path_windows(const char* path_arg_mbcs,
                               wchar_t* out_path_w, size_t out_path_w_size,
                               char* out_path_utf8, size_t out_path_utf8_size);

/**
 * @brief Prints a formatted Windows error message based on the error code.
 * @param context A string describing the context where the error occurred.
 * @param error_code The Windows error code (usually from GetLastError()).
 */
void print_win_error(const char* context, DWORD error_code);

/**
 * @brief Gets the directory containing the currently running executable.
 * @param buffer Buffer to store the UTF-8 encoded path.
 * @param buffer_size Size of the buffer.
 * @return true on success, false on failure.
 */
bool platform_get_executable_dir(char* buffer, size_t buffer_size);

#endif // _WIN32

#endif // PLATFORM_H_
