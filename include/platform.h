#ifndef PLATFORM_H_
#define PLATFORM_H_

#include <stdbool.h>
#include <stddef.h>

// --- Function Declarations ---

#ifdef _WIN32
// Forward-declare Windows types needed for the function signatures
#include <windows.h>

bool set_stdout_binary(void);
void print_win_error(const char* context, DWORD error_code);
bool get_absolute_path_windows(const char* path_arg_mbcs,
                               wchar_t* out_path_w, size_t out_path_w_size,
                               char* out_path_utf8, size_t out_path_utf8_size);
bool platform_get_executable_dir(char* buffer, size_t buffer_size);
#endif // _WIN32

#endif // PLATFORM_H_
