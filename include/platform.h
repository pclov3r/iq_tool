#ifndef PLATFORM_H_
#define PLATFORM_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
// Includes required for Windows-specific function signatures below
#include <windows.h>
// Windows implementation
#define MEMORY_BARRIER() MemoryBarrier()
#define SLEEP_MS(x) Sleep(x)
#elif defined(__GNUC__) || defined(__clang__)
#include <unistd.h>
// GCC/Clang (Linux/macOS) implementation
#define MEMORY_BARRIER() __sync_synchronize()
#define SLEEP_MS(x) usleep((x) * 1000)
#else
#error "Compiler not supported for lock-free primitives."
#endif

// --- Thread Priority Abstraction ---

/**
 * @enum ThreadPriority
 * @brief Abstract priority levels to map to OS-specific scheduling policies.
 */
typedef enum {
    PRIORITY_NORMAL,       // Default scheduling (e.g., Reader thread)
    PRIORITY_HIGH,         // Latency-sensitive DSP (e.g., Pre/Post Processor)
    PRIORITY_HIGHEST,      // Critical I/O (e.g., Disk Writer)
    PRIORITY_REALTIME      // Hardware Timing (e.g., SDR Capture)
} ThreadPriority;

/**
 * @brief Sets the priority of the calling thread.
 *
 * This function attempts to set the OS-specific priority for the current thread.
 * On Linux, it attempts Real-Time (SCHED_FIFO) scheduling first, falling back to
 * 'nice' values if permissions are missing.
 *
 * @param priority The abstract priority level to apply.
 * @param thread_name A human-readable name for the thread, used for logging warnings.
 */
void platform_set_thread_priority(ThreadPriority priority, const char* thread_name);


// --- Platform Specific Helpers ---

#ifdef _WIN32
void print_win_error(const char* context, DWORD error_code);

bool get_absolute_path_windows(const char* path_arg_mbcs,
                               wchar_t* out_path_w, size_t out_path_w_size,
                               char* out_path_utf8, size_t out_path_utf8_size);

bool platform_get_executable_dir(char* buffer, size_t buffer_size);
#endif // _WIN32

#endif // PLATFORM_H_
