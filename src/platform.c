#include "platform.h"
#include "constants.h" // <-- MODIFIED
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

// --- Platform-Specific Implementations ---

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>   // For _O_BINARY
#include <io.h>      // For _setmode
#include <shlwapi.h> // For PathIsRelativeW, PathAppendW
#include <pathcch.h> // For PathCchCombineEx

/**
 * @brief Sets stdout to binary mode on Windows.
 */
bool set_stdout_binary(void) {
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        log_error("Failed to set stdout to binary mode: %s", strerror(errno));
        return false;
    }
    return true;
}

/**
 * @brief Prints a formatted Windows error message based on the error code.
 */
void print_win_error(const char* context, DWORD error_code) {
    LPWSTR messageBuffer = NULL;
    size_t size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);
    if (messageBuffer && size > 0) {
        log_error("%hs failed. Code: %lu, Message: %ls", context, error_code, messageBuffer);
        LocalFree(messageBuffer);
    } else {
        log_error("%s failed. Code: %lu (Could not retrieve error message)", context, error_code);
    }
}

// --- MODIFIED: Rewritten to be heap-free ---
/**
 * @brief Converts an ANSI/MBCS path argument to absolute Wide and UTF-8 paths.
 */
bool get_absolute_path_windows(const char* path_arg_mbcs,
                               wchar_t* out_path_w, size_t out_path_w_size,
                               char* out_path_utf8, size_t out_path_utf8_size) {
    if (!path_arg_mbcs || !out_path_w || !out_path_utf8) return false;

    wchar_t path_arg_w[MAX_PATH_BUFFER];
    wchar_t path_to_canonicalize_w[MAX_PATH_BUFFER];

    // 1. Convert input ANSI/MBCS path argument to Wide (using system ACP)
    int required_len_w = MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, NULL, 0);
    if (required_len_w <= 0 || (size_t)required_len_w > MAX_PATH_BUFFER) {
        print_win_error("MultiByteToWideChar (get size)", GetLastError());
        return false;
    }
    if (MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, path_arg_w, required_len_w) == 0) {
        print_win_error("MultiByteToWideChar (convert)", GetLastError());
        return false;
    }

    // 2. If path is relative, combine it with the Current Working Directory
    if (PathIsRelativeW(path_arg_w)) {
        wchar_t cwd_w[MAX_PATH_BUFFER];
        DWORD cwd_len = GetCurrentDirectoryW(MAX_PATH_BUFFER, cwd_w);
        if (cwd_len == 0 || cwd_len >= MAX_PATH_BUFFER) {
            print_win_error("GetCurrentDirectoryW", GetLastError());
            return false;
        }
        HRESULT hr = PathCchCombineEx(path_to_canonicalize_w, MAX_PATH_BUFFER, cwd_w, path_arg_w, PATHCCH_ALLOW_LONG_PATHS);
        if (FAILED(hr)) {
            log_error("PathCchCombineEx failed to combine paths.");
            return false;
        }
    } else {
        wcsncpy(path_to_canonicalize_w, path_arg_w, MAX_PATH_BUFFER - 1);
        path_to_canonicalize_w[MAX_PATH_BUFFER - 1] = L'\0';
    }

    // 3. Canonicalize the path (resolve ., .. etc.) using GetFullPathNameW
    required_len_w = GetFullPathNameW(path_to_canonicalize_w, 0, NULL, NULL);
    if (required_len_w == 0 || (size_t)required_len_w > out_path_w_size) {
        print_win_error("GetFullPathNameW (get size)", GetLastError());
        return false;
    }
    if (GetFullPathNameW(path_to_canonicalize_w, required_len_w, out_path_w, NULL) == 0) {
        print_win_error("GetFullPathNameW (get path)", GetLastError());
        return false;
    }

    // 4. Convert the final Wide path to UTF-8 for internal use
    int required_len_utf8 = WideCharToMultiByte(CP_UTF8, 0, out_path_w, -1, NULL, 0, NULL, NULL);
    if (required_len_utf8 <= 0 || (size_t)required_len_utf8 > out_path_utf8_size) {
        print_win_error("WideCharToMultiByte (get size)", GetLastError());
        return false;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, out_path_w, -1, out_path_utf8, required_len_utf8, NULL, NULL) == 0) {
        print_win_error("WideCharToMultiByte (convert)", GetLastError());
        return false;
    }

    return true;
}

// --- MODIFIED: This function is now obsolete and has been removed. ---
// void free_absolute_path_windows(...) { ... }

/**
 * @brief Gets the directory containing the currently running executable.
 */
bool platform_get_executable_dir(char* buffer, size_t buffer_size) {
    wchar_t w_path[MAX_PATH_BUFFER];
    DWORD len = GetModuleFileNameW(NULL, w_path, MAX_PATH_BUFFER);
    if (len == 0 || len >= MAX_PATH_BUFFER) { // Use >= to catch truncation
        log_error("GetModuleFileNameW failed or buffer too small.");
        return false;
    }
    // Remove filename to get directory
    wchar_t* last_slash = wcsrchr(w_path, L'\\');
    if (last_slash) {
        *last_slash = L'\0';
    } else {
        // No slash, probably just the executable name, so current dir
        wcsncpy(w_path, L".", MAX_PATH_BUFFER);
        w_path[MAX_PATH_BUFFER - 1] = L'\0';
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w_path, -1, buffer, (int)buffer_size, NULL, NULL) == 0) {
        log_error("Failed to convert wide char path to UTF-8 for executable directory.");
        return false;
    }
    return true;
}

#endif // _WIN32
