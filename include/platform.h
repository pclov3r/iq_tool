#ifndef PLATFORM_H_
#define PLATFORM_H_

#include <stdbool.h> // Needed for bool return types

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
 *        Resolves relative paths against the Current Working Directory.
 * @param path_arg_mbcs The input path string (system's default ANSI code page).
 * @param absolute_path_w Pointer to receive the allocated wide character absolute path.
 * @param absolute_path_utf8 Pointer to receive the allocated UTF-8 encoded absolute path.
 * @return true on success, false on failure (e.g., memory allocation, API errors).
 * @note Caller must free the allocated strings using free_absolute_path_windows().
 */
bool get_absolute_path_windows(const char* path_arg_mbcs, wchar_t** absolute_path_w, char** absolute_path_utf8);

/**
 * @brief Frees the memory allocated by get_absolute_path_windows.
 * @param path_w Pointer to the wide character path pointer.
 * @param path_utf8 Pointer to the UTF-8 path pointer.
 * @note Sets the pointers to NULL after freeing.
 */
void free_absolute_path_windows(wchar_t** path_w, char** path_utf8);

/**
 * @brief Prints a formatted Windows error message based on the error code.
 * @param context A string describing the context where the error occurred.
 * @param error_code The Windows error code (usually from GetLastError()).
 */
void print_win_error(const char* context, DWORD error_code);

// This function is only relevant if SDRplay support is being compiled.
#if defined(WITH_SDRPLAY)
/**
 * @brief (Windows-only) Finds the full path to the correct SDRplay API DLL
 *        by searching the registry.
 * @return A dynamically allocated wide-character string containing the full path
 *         to the DLL. The caller is responsible for freeing this string with free().
 *         Returns NULL on failure.
 */
wchar_t* platform_get_sdrplay_dll_path(void);
#endif // defined(WITH_SDRPLAY)

#endif // _WIN32

#endif // PLATFORM_H_
