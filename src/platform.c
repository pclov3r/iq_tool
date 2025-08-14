#include "platform.h"
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

/**
 * @brief Converts an ANSI/MBCS path argument to absolute Wide and UTF-8 paths.
 */
bool get_absolute_path_windows(const char* path_arg_mbcs, wchar_t** absolute_path_w, char** absolute_path_utf8) {
    if (!path_arg_mbcs || !absolute_path_w || !absolute_path_utf8) return false;
    *absolute_path_w = NULL;
    *absolute_path_utf8 = NULL;

    wchar_t* path_arg_w = NULL;
    wchar_t* path_to_canonicalize_w = NULL;
    wchar_t* final_w = NULL;
    char* final_utf8 = NULL;
    bool success = false;

    // 1. Convert input ANSI/MBCS path argument to Wide (using system ACP)
    int required_len_w = MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, NULL, 0);
    if (required_len_w <= 0) {
        print_win_error("MultiByteToWideChar (get size)", GetLastError());
        goto cleanup;
    }
    path_arg_w = (wchar_t*)malloc(required_len_w * sizeof(wchar_t));
    if (!path_arg_w) { goto cleanup; }
    if (MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, path_arg_w, required_len_w) == 0) {
        print_win_error("MultiByteToWideChar (convert)", GetLastError());
        goto cleanup;
    }

    // 2. If path is relative, combine it with the Current Working Directory
    if (PathIsRelativeW(path_arg_w)) {
        wchar_t* cwd_w = NULL;
        wchar_t* combined_path_w = NULL;
        DWORD cwd_len = GetCurrentDirectoryW(0, NULL);
        if (cwd_len == 0) {
            print_win_error("GetCurrentDirectoryW (get size)", GetLastError());
            goto cleanup;
        }
        cwd_w = (wchar_t*)malloc(cwd_len * sizeof(wchar_t));
        if (!cwd_w) { goto cleanup; }
        if (GetCurrentDirectoryW(cwd_len, cwd_w) == 0) {
            print_win_error("GetCurrentDirectoryW (get dir)", GetLastError());
            free(cwd_w);
            goto cleanup;
        }
        size_t combined_len = cwd_len + wcslen(path_arg_w) + 2;
        combined_path_w = (wchar_t*)malloc(combined_len * sizeof(wchar_t));
        if (!combined_path_w) {
            free(cwd_w);
            goto cleanup;
        }
        HRESULT hr = PathCchCombineEx(combined_path_w, combined_len, cwd_w, path_arg_w, PATHCCH_ALLOW_LONG_PATHS);
        free(cwd_w);
        if (FAILED(hr)) {
            free(combined_path_w);
            goto cleanup;
        }
        path_to_canonicalize_w = combined_path_w;
    } else {
        path_to_canonicalize_w = _wcsdup(path_arg_w);
        if (!path_to_canonicalize_w) { goto cleanup; }
    }

    // 3. Canonicalize the path (resolve ., .. etc.) using GetFullPathNameW
    required_len_w = GetFullPathNameW(path_to_canonicalize_w, 0, NULL, NULL);
    if (required_len_w == 0) {
        print_win_error("GetFullPathNameW (get size)", GetLastError());
        goto cleanup;
    }
    final_w = (wchar_t*)malloc(required_len_w * sizeof(wchar_t));
    if (!final_w) { goto cleanup; }
    if (GetFullPathNameW(path_to_canonicalize_w, required_len_w, final_w, NULL) == 0) {
        print_win_error("GetFullPathNameW (get path)", GetLastError());
        goto cleanup;
    }

    // 4. Convert the final Wide path to UTF-8 for internal use (e.g., libsndfile)
    int required_len_utf8 = WideCharToMultiByte(CP_UTF8, 0, final_w, -1, NULL, 0, NULL, NULL);
    if (required_len_utf8 <= 0) {
        print_win_error("WideCharToMultiByte (get size)", GetLastError());
        goto cleanup;
    }
    final_utf8 = (char*)malloc(required_len_utf8 * sizeof(char));
    if (!final_utf8) { goto cleanup; }
    if (WideCharToMultiByte(CP_UTF8, 0, final_w, -1, final_utf8, required_len_utf8, NULL, NULL) == 0) {
        print_win_error("WideCharToMultiByte (convert)", GetLastError());
        free(final_utf8);
        final_utf8 = NULL;
        goto cleanup;
    }

    // 5. Set output pointers and mark success
    *absolute_path_w = final_w;
    *absolute_path_utf8 = final_utf8;
    success = true;

cleanup:
    free(path_arg_w);
    free(path_to_canonicalize_w);
    if (!success) {
        free(final_w);
        free(final_utf8);
        *absolute_path_w = NULL;
        *absolute_path_utf8 = NULL;
    }
    return success;
}

/**
 * @brief Frees the path pair allocated by get_absolute_path_windows.
 */
void free_absolute_path_windows(wchar_t** path_w, char** path_utf8) {
    if (path_w && *path_w) {
        free(*path_w);
        *path_w = NULL;
    }
    if (path_utf8 && *path_utf8) {
        free(*path_utf8);
        *path_utf8 = NULL;
    }
}

// This function is only compiled if SDRplay support is enabled via CMake.
#if defined(WITH_SDRPLAY)
/**
 * @brief (Windows-only) Finds the full path to the correct SDRplay API DLL
 *        by searching the registry.
 */
wchar_t* platform_get_sdrplay_dll_path(void) {
    HKEY hKey;
    LONG reg_status;
    wchar_t api_path_buf[MAX_PATH] = {0};
    DWORD buffer_size = sizeof(api_path_buf);
    bool path_found = false;

    // Try the standard 64-bit registry location first
    reg_status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SDRplay\\Service\\API", 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (reg_status == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"Install_Dir", NULL, NULL, (LPBYTE)api_path_buf, &buffer_size) == ERROR_SUCCESS) {
            path_found = true;
        }
        RegCloseKey(hKey);
    }

    // If not found, try the WOW6432Node for 32-bit installers on 64-bit systems
    if (!path_found) {
        reg_status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\SDRplay\\Service\\API", 0, KEY_READ, &hKey);
        if (reg_status == ERROR_SUCCESS) {
            buffer_size = sizeof(api_path_buf); // Reset buffer size for the new call
            if (RegQueryValueExW(hKey, L"Install_Dir", NULL, NULL, (LPBYTE)api_path_buf, &buffer_size) == ERROR_SUCCESS) {
                path_found = true;
            }
            RegCloseKey(hKey);
        }
    }

    if (!path_found) {
        log_error("Could not find SDRplay API installation path in the registry.");
        log_error("Please ensure the SDRplay API service is installed correctly.");
        return NULL;
    }

    // Append the architecture-specific subfolder and DLL name
#ifdef _WIN64
    PathAppendW(api_path_buf, L"x64");
#else
    PathAppendW(api_path_buf, L"x86");
#endif
    PathAppendW(api_path_buf, L"sdrplay_api.dll");

    // Return a dynamically allocated copy of the path that the caller must free
    return _wcsdup(api_path_buf);
}
#endif // defined(WITH_SDRPLAY)

#endif // _WIN32
