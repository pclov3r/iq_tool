/**
 * @file platform.c
 */

#include "platform.h"
#include "constants.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shlwapi.h>
#include <pathcch.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

void platform_set_thread_priority(ThreadPriority priority, const char* thread_name) {
#ifdef _WIN32
    // --- Windows Implementation ---
    int win_prio = THREAD_PRIORITY_NORMAL;
    const char* prio_desc = "Normal";

    switch (priority) {
        case PRIORITY_REALTIME:
            win_prio = THREAD_PRIORITY_TIME_CRITICAL;
            prio_desc = "Time Critical";
            break;
        case PRIORITY_HIGHEST:
            win_prio = THREAD_PRIORITY_HIGHEST;
            prio_desc = "Highest";
            break;
        case PRIORITY_HIGH:
            win_prio = THREAD_PRIORITY_ABOVE_NORMAL;
            prio_desc = "Above Normal";
            break;
        default: break;
    }

    if (!SetThreadPriority(GetCurrentThread(), win_prio)) {
        log_warn("Failed to set '%s' thread scheduling priority to %s.", thread_name, prio_desc);
    }
#else
    // --- Linux / POSIX Implementation ---
    int policy = SCHED_OTHER;
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    int nice_val = 0;
    const char* prio_desc = "Normal";

    switch (priority) {
        case PRIORITY_REALTIME:
            policy = SCHED_FIFO;
            param.sched_priority = 50;
            nice_val = -15;
            prio_desc = "Realtime (FIFO)";
            break;

        case PRIORITY_HIGHEST:
            policy = SCHED_FIFO;
            param.sched_priority = 20;
            nice_val = -10;
            prio_desc = "Highest (FIFO)";
            break;

        case PRIORITY_HIGH:
            policy = SCHED_FIFO;
            param.sched_priority = 10;
            nice_val = -5;
            prio_desc = "High (FIFO)";
            break;

        default:
            return;
    }

    // 1. Attempt FIFO Scheduling
    int fifo_err = pthread_setschedparam(pthread_self(), policy, &param);
    if (fifo_err == 0) {
        log_debug("Set '%s' thread scheduling priority to %s.", thread_name, prio_desc);
        return;
    }

    // Report why we are falling back
    log_debug("Failed to set '%s' to %s (%s). Attempting Nice fallback...",
              thread_name, prio_desc, strerror(fifo_err));

    // 2. Attempt Nice fallback
    pid_t tid = (pid_t)syscall(SYS_gettid);
    if (setpriority(PRIO_PROCESS, tid, nice_val) == 0) {
        log_debug("Set '%s' thread scheduling priority to %s (Nice Fallback).", thread_name, prio_desc);
        return;
    }

    // 3. Both failed - print a single, clean warning
    log_warn("Failed to elevate '%s' thread scheduling priority to %s: %s",
             thread_name, prio_desc, strerror(fifo_err));
#endif
}

#ifdef _WIN32

void print_win_error(const char* context, DWORD error_code) {
    LPWSTR messageBuffer = NULL;
    size_t size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);
    if (size > 0) {
        log_error("%s failed. Code: %lu, Message: %ls", context, error_code, messageBuffer);
        LocalFree(messageBuffer);
    } else {
        log_error("%s failed. Code: %lu (Could not retrieve error message)", context, error_code);
    }
}

bool get_absolute_path_windows(const char* path_arg_mbcs,
                               wchar_t* out_path_w, size_t out_path_w_size,
                               char* out_path_utf8, size_t out_path_utf8_size) {
    if (!path_arg_mbcs || !out_path_w || !out_path_utf8) return false;

    wchar_t path_arg_w[MAX_PATH_BUFFER];
    wchar_t path_to_canonicalize_w[MAX_PATH_BUFFER];

    int required_length_w = MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, NULL, 0);
    if (required_length_w <= 0 || (size_t)required_length_w > MAX_PATH_BUFFER) {
        print_win_error("MultiByteToWideChar (get size)", GetLastError());
        return false;
    }
    if (MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, path_arg_w, required_length_w) == 0) {
        print_win_error("MultiByteToWideChar (convert)", GetLastError());
        return false;
    }

    if (PathIsRelativeW(path_arg_w)) {
        wchar_t cwd_w[MAX_PATH_BUFFER];
        DWORD cwd_length = GetCurrentDirectoryW(MAX_PATH_BUFFER, cwd_w);
        if (cwd_length == 0 || cwd_length >= MAX_PATH_BUFFER) {
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

    required_length_w = GetFullPathNameW(path_to_canonicalize_w, 0, NULL, NULL);
    if (required_length_w == 0 || (size_t)required_length_w > out_path_w_size) {
        print_win_error("GetFullPathNameW (get size)", GetLastError());
        return false;
    }
    if (GetFullPathNameW(path_to_canonicalize_w, required_length_w, out_path_w, NULL) == 0) {
        print_win_error("GetFullPathNameW (get path)", GetLastError());
        return false;
    }

    int required_length_utf8 = WideCharToMultiByte(CP_UTF8, 0, out_path_w, -1, NULL, 0, NULL, NULL);
    if (required_length_utf8 <= 0 || (size_t)required_length_utf8 > out_path_utf8_size) {
        print_win_error("WideCharToMultiByte (get size)", GetLastError());
        return false;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, out_path_w, -1, out_path_utf8, required_length_utf8, NULL, NULL) == 0) {
        print_win_error("WideCharToMultiByte (convert)", GetLastError());
        return false;
    }

    return true;
}

bool platform_get_executable_dir(char* buffer, size_t buffer_size) {
    wchar_t w_path[MAX_PATH_BUFFER];
    DWORD length = GetModuleFileNameW(NULL, w_path, MAX_PATH_BUFFER);
    if (length == 0 || length >= MAX_PATH_BUFFER) {
        log_error("GetModuleFileNameW failed or buffer too small.");
        return false;
    }
    wchar_t* last_slash = wcsrchr(w_path, L'\\');
    if (last_slash) {
        *last_slash = L'\0';
    } else {
        wcsncpy(w_path, L".", MAX_PATH_BUFFER);
        w_path[MAX_PATH_BUFFER - 1] = L'\0';
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w_path, -1, buffer, (int)buffer_size, NULL, NULL) == 0) {
        log_error("Failed to convert wide char path to UTF-8 for executable directory.");
        return false;
    }
    return true;
}

#endif

// --- CPU Feature Diagnostics ---
void platform_check_cpu_features(void) {
#if defined(__GNUC__) || defined(__clang__)

    // Intel/AMD x86_64 Diagnostics
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();

    // 1. If this is an AVX2 build, directly check if the hardware supports it
    #if defined(__AVX2__)
        if (!__builtin_cpu_supports("avx2")) {
            #ifdef _WIN32
                log_fatal("Error: This binary uses AVX2 instructions, but your processor only supports AVX. Please download the AVX release build.");
            #else
                log_fatal("Error: This binary uses AVX2 instructions, but your processor only supports AVX. Please recompile the software using the default Release build option without passing any custom CPU optimization flags.");
            #endif
            exit(EXIT_FAILURE);
        }
        
    // 2. If this is a standard AVX build, directly check hardware and warn if they could upgrade
    #elif defined(__AVX__)
        if (!__builtin_cpu_supports("avx")) {
            #ifdef _WIN32
                log_fatal("Error: This build of the application uses AVX instructions, which your processor does not support. Please rebuild the software yourself without AVX optimizations to run on this machine.");
            #else
                log_fatal("Error: This build of the application uses AVX instructions, which your processor does not support. Please recompile the software using the default Release build option without passing any custom CPU optimization flags.");
            #endif
            exit(EXIT_FAILURE);
        }
        if (__builtin_cpu_supports("avx2")) {
            #ifdef _WIN32
                log_warn("Notice: Your processor supports AVX2 instructions, but you are using the AVX build. Download the AVX2 release build for better performance.");
            #else
                log_warn("Notice: Your processor supports AVX2 instructions, but this binary is only using AVX instructions. Consider recompiling the software using the default Release build option without passing any custom CPU optimization flags for better performance.");
            #endif
        }

    // 3. If someone compiled a BASELINE build (No AVX at all)
    #else
        if (__builtin_cpu_supports("avx2")) {
            #ifdef _WIN32
                log_warn("Notice: Your processor supports AVX2 instructions, but this binary is not using any AVX optimizations. Download the AVX2 release build for better performance.");
            #else
                log_warn("Notice: Your processor supports AVX2 instructions, but this binary is not using any AVX optimizations. Consider recompiling the software using the default Release build option without passing any custom CPU optimization flags for better performance.");
            #endif
        } else if (__builtin_cpu_supports("avx")) {
            #ifdef _WIN32
                log_warn("Notice: Your processor supports AVX instructions, but this binary is not using any AVX optimizations. Download the AVX release build for better performance.");
            #else
                log_warn("Notice: Your processor supports AVX instructions, but this binary is not using any AVX optimizations. Consider recompiling the software using the default Release build option without passing any custom CPU optimization flags for better performance.");
            #endif
        }
    #endif

    // ARM Architecture Diagnostics (NEON)
#elif defined(__aarch64__)
    #ifndef __ARM_NEON
        #ifdef _WIN32
            log_warn("Notice: Your processor supports NEON instructions, but this binary is not using any NEON optimizations. Download the native ARM NEON release build for better performance.");
        #else
            log_warn("Notice: Your processor supports NEON instructions, but this binary is not using any NEON optimizations. Consider recompiling the software using the default Release build option without passing any custom CPU optimization flags for better performance.");
        #endif
    #endif
#endif

#endif
}
