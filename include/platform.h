/**
 * @file platform.h
 */

#ifndef PLATFORM_H_
#define PLATFORM_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
// Includes required for Windows-specific function signatures below
#include <io.h>
#include <malloc.h>
#include <windows.h>
#define strcasecmp _stricmp
#define platform_write(fd, buf, count)                                         \
  _write((fd), (buf), (unsigned int)(count))
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
#define platform_write(fd, buf, count) write((fd), (buf), (count))
#else
#error "Compiler not supported for lock-free primitives."
#endif

// --- Timing & Execution Delay ---

/**
 * @brief Suspends execution of the calling thread for the specified duration.
 * @param ms Duration to sleep in milliseconds.
 */
void platform_sleep(unsigned int ms);

/**
 * @brief Returns high-resolution monotonic time in seconds.
 *
 * Uses QueryPerformanceCounter on Windows and clock_gettime(CLOCK_MONOTONIC)
 * on POSIX.
 */
double platform_get_time(void);

// --- File Stream & Descriptor I/O ---

/**
 * @brief Sets a standard file stream to binary mode on Windows.
 * On POSIX systems, streams are always binary, so this is a no-op returning
 * true.
 * @param stream File stream to configure (e.g. stdin, stdout).
 * @return true on success or if unneeded, false on error.
 */
bool platform_set_binary_mode(FILE *stream);

/**
 * @brief Opens a file from a UTF-8 path.
 *
 * On Windows, converts UTF-8 path and mode to wide characters and uses _wfopen.
 * On POSIX, calls fopen directly.
 *
 * @param path UTF-8 path to the file.
 * @param mode Open mode string (e.g. "r", "wb").
 * @return Opened FILE pointer, or NULL on failure.
 */
FILE *platform_fopen(const char *path, const char *mode);

/**
 * @brief Checks if a path exists and points to a safe regular file.
 *
 * On Windows, verifies the path exists and is not a directory or reparse point.
 * On POSIX, uses lstat to ensure the file is a regular file (not a
 * symlink/dir).
 *
 * @param path UTF-8 path to check.
 * @return true if the file exists and is regular, false otherwise.
 */
bool platform_is_file(const char *path);

/**
 * @brief Checks if a path exists and is a directory.
 *
 * @param path UTF-8 path to check.
 * @return true if the path exists and is a directory, false otherwise.
 */
typedef enum {
  PLATFORM_FILE_OK = 0,
  PLATFORM_FILE_IS_DIRECTORY,
  PLATFORM_FILE_STATUS_ERROR
} PlatformFileStatus;

/**
 * @brief Returns a human-readable description for a PlatformFileStatus code.
 */
const char *platform_file_status_str(PlatformFileStatus status);

/**
 * @brief Verifies that an open FILE handle represents a regular file.
 *
 * Checks that the underlying descriptor is valid and not a directory.
 *
 * @param fp An open FILE handle to check.
 * @return PLATFORM_FILE_OK on success, PLATFORM_FILE_IS_DIRECTORY if a
 * directory was opened, or PLATFORM_FILE_STATUS_ERROR on stat failure.
 */
PlatformFileStatus platform_file_verify(FILE *fp);

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

// --- Configuration & Search Paths ---

struct MemoryArena;

/**
 * @brief Retrieves platform-specific directory search paths for configuration
 * files.
 *
 * @param paths Array of string pointers to populate.
 * @param max_paths Maximum number of paths that can be stored in the array.
 * @param arena Memory arena to allocate path strings from.
 * @return Number of search paths added to the array.
 */
size_t platform_get_config_search_paths(const char **paths, size_t max_paths,
                                        struct MemoryArena *arena);

// --- CPU Feature Diagnostics ---

/**
 * @brief Safely checks if the host CPU meets the binary's compiler-enforced
 * requirements.
 *
 * If the binary was compiled for AVX or AVX2, this validates that both the
 * hardware and OS support the required instruction sets at runtime to prevent
 * Illegal Instruction crashes.
 */
void platform_check_cpu_features(void);

#endif // PLATFORM_H_
