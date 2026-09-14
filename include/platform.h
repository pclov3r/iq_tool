/**
 * @file platform.h
 */

#ifndef PLATFORM_H_
#define PLATFORM_H_
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
// Includes required for Windows-specific function signatures below
#include <malloc.h>
#include <windows.h>
#define strcasecmp _stricmp
// MinGW doesn't expose aligned_alloc - provide a compatibility shim via macro.
// Note: _aligned_malloc argument order is (size, alignment), opposite of
// aligned_alloc.
#define aligned_alloc(alignment, size) _aligned_malloc((size), (alignment))
#define aligned_free(ptr) _aligned_free((ptr))
#elif defined(__GNUC__) || defined(__clang__)
#include <strings.h>
#include <unistd.h>
// GCC/Clang (Linux/macOS) implementation
#define aligned_free(ptr) free((ptr))
#else
#error "Compiler not supported for lock-free primitives."
#endif

/**
 * @brief Suspends execution of the calling thread for the specified duration.
 * @param ms Duration to sleep in milliseconds.
 */
void platform_sleep(unsigned int ms);

// --- Thread Priority Abstraction ---

/**
 * @enum ThreadPriority
 * @brief Abstract priority levels to map to OS-specific scheduling policies.
 */
typedef enum {
  PRIORITY_NORMAL,  // Default scheduling (e.g., Chunker thread)
  PRIORITY_HIGH,    // Latency-sensitive DSP (e.g., Pre/Post Processor)
  PRIORITY_HIGHEST, // Critical I/O (e.g., Disk Writer)
  PRIORITY_REALTIME // Hardware Timing (e.g., Input SDR driver)
} ThreadPriority;

/**
 * @brief Sets the priority of the calling thread.
 *
 * This function attempts to set the OS-specific priority for the current
 * thread. On Linux, it attempts Real-Time (SCHED_FIFO) scheduling first,
 * falling back to 'nice' values if permissions are missing.
 *
 * @param priority The abstract priority level to apply.
 * @param thread_name A human-readable name for the thread, used for logging
 * warnings.
 */
void platform_set_thread_priority(ThreadPriority priority,
                                  const char *thread_name);

/**
 * @brief Safely checks if the host CPU meets the binary's compiler-enforced
 * requirements.
 *
 * If the binary was compiled for AVX or AVX2, this validates that both the
 * hardware and OS support the required instruction sets at runtime to prevent
 * Illegal Instruction crashes.
 */
void platform_check_cpu_features(void);

// --- Dynamic Library Loading ---

void *platform_dll_load(const char *dll_path);
#ifdef _WIN32
void *platform_dll_load_w(const wchar_t *dll_path);
#endif
void *platform_dll_get_symbol(void *handle, const char *symbol_name);
void platform_dll_unload(void *handle);

#define DLL_LOAD_FUNCTION(dll_handle, api_struct, base_prefix, func_name)      \
  do {                                                                         \
    void *proc = platform_dll_get_symbol(dll_handle, base_prefix #func_name);  \
    if (!proc) {                                                               \
      log_fatal("Failed to load DLL function: %s%s", base_prefix, #func_name); \
      platform_dll_unload(dll_handle);                                         \
      dll_handle = NULL;                                                       \
      return false;                                                            \
    }                                                                          \
    memcpy(&(api_struct).func_name, &proc, sizeof((api_struct).func_name));    \
  } while (0)

// --- Platform Specific Helpers ---

#ifdef _WIN32
void print_win_error(const char *context, DWORD error_code);

bool get_absolute_path_windows(const char *path_arg_mbcs, wchar_t *out_path_w,
                               size_t out_path_w_size, char *out_path_utf8,
                               size_t out_path_utf8_size);

bool platform_get_executable_dir(char *buffer, size_t buffer_size);
#endif // _WIN32

#endif // PLATFORM_H_
