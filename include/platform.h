/**
 * @file platform.h
 */

#ifndef PLATFORM_H_
#define PLATFORM_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
// Includes required for Windows-specific function signatures below
#include <io.h>
#include <malloc.h>
#include <sndfile.h>
#include <windows.h>
#define strcasecmp _stricmp
#define platform_write(fd, buf, count)                                         \
  _write((fd), (buf), (unsigned int)(count))
// MinGW doesn't expose aligned_alloc - provide a compatibility shim via macro.
// Note: _aligned_malloc argument order is (size, alignment), opposite of
// aligned_alloc.
#define aligned_alloc(alignment, size) _aligned_malloc((size), (alignment))
#define aligned_free(ptr) _aligned_free((ptr))

// Transparent Windows libsndfile shim: converts UTF-8 -> wchar_t and calls
// sf_wchar_open
static inline SNDFILE *win32_compat_sf_open(const char *utf8_path, int mode,
                                            SF_INFO *sfinfo) {
  if (!utf8_path || !sfinfo)
    return NULL;
  wchar_t path_w[4096];
  if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, path_w, 4096) <= 0)
    return NULL;
  return sf_wchar_open(path_w, mode, sfinfo);
}
#define sf_open(path, mode, sfinfo)                                            \
  win32_compat_sf_open((path), (mode), (sfinfo))
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

/**
 * @brief Suspends execution of the calling thread for the specified microsecond
 * duration.
 * @param us Duration to sleep in microseconds.
 */
void platform_sleep_us(unsigned int us);

/**
 * @brief Converts broken-down time in UTC to calendar time (seconds since Unix
 * epoch).
 *
 * Portable equivalent to POSIX timegm / Windows _mkgmtime.
 *
 * @param tm Pointer to broken-down time struct (interpreted as UTC).
 * @return Time in seconds, or (time_t)-1 on error.
 */
time_t platform_timegm(struct tm *tm);

/**
 * @brief Converts calendar time to broken-down UTC time representation in a
 * thread-safe manner.
 *
 * Portable equivalent to POSIX gmtime_r / Windows gmtime_s.
 *
 * @param timep Pointer to time_t to convert.
 * @param result Pointer to struct tm where result will be written.
 * @return Pointer to result on success, or NULL on failure.
 */
struct tm *platform_gmtime_r(const time_t *timep, struct tm *result);

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
 * @brief Sets a file descriptor to binary mode on Windows.
 * On POSIX systems, descriptors are always binary, so this is a no-op returning
 * true.
 * @param fd File descriptor to configure.
 * @return true on success or if unneeded, false on error.
 */
bool platform_set_binary_mode_fd(int fd);

/**
 * @brief Checks if a file descriptor is open and valid.
 * @param fd File descriptor to test.
 * @return true if valid, false if closed or invalid.
 */
bool platform_is_fd_valid(int fd);

/**
 * @brief Writes raw bytes directly to standard error, safe for emergency exits.
 * @param buffer String buffer to write.
 * @param len Number of bytes to write.
 */
void platform_write_stderr(const char *buffer, size_t len);

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
 * @brief Opens a file for reading text or binary data.
 *
 * Automatically handles UTF-8 path conversion across Windows and POSIX.
 *
 * @param path UTF-8 path to the file.
 * @return Opened FILE handle, or NULL on failure.
 */
FILE *platform_file_open_read(const char *path);

/**
 * @brief Opens or creates a binary file for writing, truncating any existing
 * content.
 *
 * Automatically handles UTF-8 path conversion across Windows and POSIX.
 *
 * @param path UTF-8 path to the file.
 * @return Opened FILE handle, or NULL on failure.
 */
FILE *platform_file_open_write(const char *path);

/**
 * @brief Returns the file size in bytes for a given path.
 *
 * Handles UTF-8 paths across Windows and POSIX.
 *
 * @param path UTF-8 path to the file.
 * @return File size in bytes, or -1 on error.
 */
int64_t platform_file_size(const char *path);

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
bool platform_is_directory(const char *path);

/**
 * @brief Returns a pointer to the filename component of a path, skipping
 * leading directory components separated by '/' or '\\'.
 *
 * @param path Path string.
 * @return Pointer inside path to the basename, or NULL if path is NULL.
 */
const char *platform_get_basename(const char *path);

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

bool platform_get_executable_dir(char *buffer, size_t buffer_size);
#endif // _WIN32

struct MemoryArena;

/**
 * @brief Resolves a relative or user-supplied file path into a canonical
 * absolute path string allocated from the provided arena.
 *
 * For input files (is_input = true), verifies that the file exists and is
 * regular. For output files (is_input = false), verifies that the parent
 * directory exists.
 *
 * @param path The raw user-supplied path string.
 * @param is_input true if validating an existing input file, false for output
 * files.
 * @param arena Memory arena to allocate the resolved path string from.
 * @return Resolved absolute UTF-8 path string, or NULL on error.
 */
char *platform_resolve_path(const char *path, bool is_input,
                            struct MemoryArena *arena);

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
