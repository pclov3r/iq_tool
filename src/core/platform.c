/**
 * @file platform.c
 * @brief Cross-platform system helpers, terminal controls, and thread priority
 * abstraction.
 */

#include "platform.h"
#include "config/constants.h"
#include "log.h"
#include "mem_arena.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <pathcch.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// --- Timing & Execution Delay ---

void platform_sleep(unsigned int ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  usleep((useconds_t)ms * 1000);
#endif
}

double platform_get_time(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, count;
  if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&count)) {
    return (double)count.QuadPart / (double)freq.QuadPart;
  }
  // Fallback to a lower-resolution timer if QPC fails
  return (double)GetTickCount64() / 1000.0;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
  }
  // Fallback for systems without clock_gettime
  return (double)time(NULL);
#endif
}

// --- File Stream & Descriptor I/O ---

bool platform_set_binary_mode(FILE *stream) {
#ifdef _WIN32
  if (!stream)
    return false;
  int fd = _fileno(stream);
  if (fd == -1)
    return false;
  return _setmode(fd, _O_BINARY) != -1;
#else
  (void)stream;
  return true;
#endif
}

FILE *platform_fopen(const char *path, const char *mode) {
  if (!path || !mode)
    return NULL;
#ifdef _WIN32
  wchar_t path_w[APP_MAX_PATH_BUFFER];
  wchar_t mode_w[32];
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, path_w, APP_MAX_PATH_BUFFER) <=
      0)
    return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, mode_w, 32) <= 0)
    return NULL;
  return _wfopen(path_w, mode_w);
#else
  return fopen(path, mode);
#endif
}

FILE *platform_file_open_read(const char *path) {
  return platform_fopen(path, "r");
}

FILE *platform_file_open_write(const char *path) {
  return platform_fopen(path, "wb");
}

int64_t platform_file_size(const char *path) {
  if (!path || *path == '\0')
    return -1;
#ifdef _WIN32
  wchar_t path_w[APP_MAX_PATH_BUFFER];
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, path_w, APP_MAX_PATH_BUFFER) <=
      0)
    return -1;
  struct __stat64 st;
  if (_wstat64(path_w, &st) == 0)
    return (int64_t)st.st_size;
  return -1;
#else
  struct stat st;
  if (stat(path, &st) == 0)
    return (int64_t)st.st_size;
  return -1;
#endif
}

bool platform_is_file(const char *path) {
  if (!path || *path == '\0')
    return false;
#ifdef _WIN32
  wchar_t path_w[APP_MAX_PATH_BUFFER];
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, path_w, APP_MAX_PATH_BUFFER) <=
      0)
    return false;
  DWORD attrs = GetFileAttributesW(path_w);
  if (attrs == INVALID_FILE_ATTRIBUTES)
    return false;
  return !(attrs & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  struct stat st;
  if (lstat(path, &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
#endif
}

const char *platform_file_status_str(PlatformFileStatus status) {
  switch (status) {
  case PLATFORM_FILE_OK:
    return "OK";
  case PLATFORM_FILE_IS_DIRECTORY:
    return "Path is a directory";
  case PLATFORM_FILE_STATUS_ERROR:
    return "Could not retrieve file status";
  default:
    return "Unknown file error";
  }
}

PlatformFileStatus platform_file_verify(FILE *fp) {
  if (!fp)
    return PLATFORM_FILE_STATUS_ERROR;
#ifdef _WIN32
  int fd = _fileno(fp);
  if (fd == -1)
    return PLATFORM_FILE_STATUS_ERROR;
  HANDLE hFile = (HANDLE)_get_osfhandle(fd);
  if (hFile == INVALID_HANDLE_VALUE)
    return PLATFORM_FILE_STATUS_ERROR;
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(hFile, &info))
    return PLATFORM_FILE_STATUS_ERROR;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    return PLATFORM_FILE_IS_DIRECTORY;
  return PLATFORM_FILE_OK;
#else
  int fd = fileno(fp);
  if (fd == -1)
    return PLATFORM_FILE_STATUS_ERROR;
  struct stat st;
  if (fstat(fd, &st) != 0)
    return PLATFORM_FILE_STATUS_ERROR;
  if (S_ISDIR(st.st_mode))
    return PLATFORM_FILE_IS_DIRECTORY;
  return PLATFORM_FILE_OK;
#endif
}

// --- Thread Priority Abstraction ---

void platform_set_thread_priority(ThreadPriority priority,
                                  const char *thread_name) {
#ifdef _WIN32
  // --- Windows Implementation ---
  int win_prio = THREAD_PRIORITY_NORMAL;
  const char *prio_desc = "Normal";

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
  default:
    break;
  }

  if (!SetThreadPriority(GetCurrentThread(), win_prio)) {
    log_warn("Failed to set '%s' thread scheduling priority to %s.",
             thread_name, prio_desc);
  }
#else
  // --- Linux / POSIX Implementation ---
  int policy = SCHED_OTHER;
  struct sched_param param;
  memset(&param, 0, sizeof(param));
  int nice_val = 0;
  const char *prio_desc = "Normal";

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
    log_debug("Set '%s' thread scheduling priority to %s.", thread_name,
              prio_desc);
    return;
  }

  // 2. Attempt Nice fallback
  bool nice_ok = false;
#if defined(__linux__) && defined(SYS_gettid)
  pid_t tid = (pid_t)syscall(SYS_gettid);
  nice_ok = (setpriority(PRIO_PROCESS, (id_t)tid, nice_val) == 0);
#elif defined(__APPLE__) && defined(PRIO_DARWIN_THREAD)
  nice_ok = (setpriority(PRIO_DARWIN_THREAD, 0, nice_val) == 0);
#elif defined(__FreeBSD__)
  nice_ok = (setpriority(PRIO_PROCESS, (id_t)pthread_getthreadid_np(),
                         nice_val) == 0);
#else
  (void)nice_val;
#endif

  if (nice_ok) {
    log_debug("Set '%s' thread scheduling priority to %s.", thread_name,
              prio_desc);
    return;
  }

  // 3. Both failed - print a single, clean warning
  log_warn("Failed to elevate '%s' thread scheduling priority to %s: %s",
           thread_name, prio_desc, strerror(fifo_err));
#endif
}

// --- Windows Platform Helpers ---

#ifdef _WIN32

void print_win_error(const char *context, DWORD error_code) {
  LPWSTR messageBuffer = NULL;
  size_t size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPWSTR)&messageBuffer, 0, NULL);
  if (size > 0) {
    log_error("%s failed. Code: %lu, Message: %ls", context, error_code,
              messageBuffer);
    LocalFree(messageBuffer);
  } else {
    log_error("%s failed. Code: %lu (Could not retrieve error message)",
              context, error_code);
  }
}

bool get_absolute_path_windows(const char *path_arg_mbcs, wchar_t *out_path_w,
                               size_t out_path_w_size, char *out_path_utf8,
                               size_t out_path_utf8_size) {
  if (!path_arg_mbcs || !out_path_w || !out_path_utf8)
    return false;

  wchar_t path_arg_w[APP_MAX_PATH_BUFFER];
  wchar_t path_to_canonicalize_w[APP_MAX_PATH_BUFFER];

  int required_length_w =
      MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, NULL, 0);
  if (required_length_w <= 0 ||
      (size_t)required_length_w > APP_MAX_PATH_BUFFER) {
    print_win_error("MultiByteToWideChar (get size)", GetLastError());
    return false;
  }
  if (MultiByteToWideChar(CP_ACP, 0, path_arg_mbcs, -1, path_arg_w,
                          required_length_w) == 0) {
    print_win_error("MultiByteToWideChar (convert)", GetLastError());
    return false;
  }

  if (PathIsRelativeW(path_arg_w)) {
    wchar_t cwd_w[APP_MAX_PATH_BUFFER];
    DWORD cwd_length = GetCurrentDirectoryW(APP_MAX_PATH_BUFFER, cwd_w);
    if (cwd_length == 0 || cwd_length >= APP_MAX_PATH_BUFFER) {
      print_win_error("GetCurrentDirectoryW", GetLastError());
      return false;
    }
    HRESULT hr = PathCchCombineEx(path_to_canonicalize_w, APP_MAX_PATH_BUFFER,
                                  cwd_w, path_arg_w, PATHCCH_ALLOW_LONG_PATHS);
    if (FAILED(hr)) {
      log_error("PathCchCombineEx failed to combine paths.");
      return false;
    }
  } else {
    wcsncpy(path_to_canonicalize_w, path_arg_w, APP_MAX_PATH_BUFFER - 1);
    path_to_canonicalize_w[APP_MAX_PATH_BUFFER - 1] = L'\0';
  }

  required_length_w = GetFullPathNameW(path_to_canonicalize_w, 0, NULL, NULL);
  if (required_length_w == 0 || (size_t)required_length_w > out_path_w_size) {
    print_win_error("GetFullPathNameW (get size)", GetLastError());
    return false;
  }
  if (GetFullPathNameW(path_to_canonicalize_w, required_length_w, out_path_w,
                       NULL) == 0) {
    print_win_error("GetFullPathNameW (get path)", GetLastError());
    return false;
  }

  int required_length_utf8 =
      WideCharToMultiByte(CP_UTF8, 0, out_path_w, -1, NULL, 0, NULL, NULL);
  if (required_length_utf8 <= 0 ||
      (size_t)required_length_utf8 > out_path_utf8_size) {
    print_win_error("WideCharToMultiByte (get size)", GetLastError());
    return false;
  }
  if (WideCharToMultiByte(CP_UTF8, 0, out_path_w, -1, out_path_utf8,
                          required_length_utf8, NULL, NULL) == 0) {
    print_win_error("WideCharToMultiByte (convert)", GetLastError());
    return false;
  }

  return true;
}

bool platform_get_executable_dir(char *buffer, size_t buffer_size) {
  wchar_t w_path[APP_MAX_PATH_BUFFER];
  DWORD length = GetModuleFileNameW(NULL, w_path, APP_MAX_PATH_BUFFER);
  if (length == 0 || length >= APP_MAX_PATH_BUFFER) {
    log_error("GetModuleFileNameW failed or buffer too small.");
    return false;
  }
  wchar_t *last_slash = wcsrchr(w_path, L'\\');
  if (last_slash) {
    *last_slash = L'\0';
  } else {
    wcsncpy(w_path, L".", APP_MAX_PATH_BUFFER);
    w_path[APP_MAX_PATH_BUFFER - 1] = L'\0';
  }
  if (WideCharToMultiByte(CP_UTF8, 0, w_path, -1, buffer, (int)buffer_size,
                          NULL, NULL) == 0) {
    log_error(
        "Failed to convert wide char path to UTF-8 for executable directory.");
    return false;
  }
  return true;
}

#endif // _WIN32

// --- Dynamic Library Loading ---

void *platform_dll_load(const char *dll_path) {
#ifdef _WIN32
  HMODULE handle = LoadLibraryA(dll_path);
  if (!handle) {
    print_win_error("LoadLibraryA", GetLastError());
  }
  return (void *)handle;
#else
  void *handle = dlopen(dll_path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    log_error("dlopen failed: %s", dlerror());
  }
  return handle;
#endif
}

#ifdef _WIN32
void *platform_dll_load_w(const wchar_t *dll_path) {
  HMODULE handle = LoadLibraryW(dll_path);
  if (!handle) {
    print_win_error("LoadLibraryW", GetLastError());
  }
  return (void *)handle;
}
#endif

void *platform_dll_get_symbol(void *handle, const char *symbol_name) {
  if (!handle)
    return NULL;
#ifdef _WIN32
  /* Cast through size_t to silence ISO C -Wpedantic warnings about converting
   * function pointers to object pointers */
  return (void *)(size_t)GetProcAddress((HMODULE)handle, symbol_name);
#else
  return dlsym(handle, symbol_name);
#endif
}

void platform_dll_unload(void *handle) {
  if (!handle)
    return;
#ifdef _WIN32
  FreeLibrary((HMODULE)handle);
#else
  dlclose(handle);
#endif
}

// --- Configuration & Search Paths ---

size_t platform_get_config_search_paths(const char **paths, size_t max_paths,
                                        struct MemoryArena *arena) {
  if (!paths || max_paths == 0)
    return 0;

  size_t count = 0;

#ifdef _WIN32
  char exe_dir[APP_MAX_PATH_BUFFER];
  if (platform_get_executable_dir(exe_dir, sizeof(exe_dir))) {
    char *exe_dir_copy =
        arena ? (char *)mem_arena_alloc(arena, strlen(exe_dir) + 1, false)
              : NULL;
    if (exe_dir_copy) {
      strcpy(exe_dir_copy, exe_dir);
      if (count < max_paths)
        paths[count++] = exe_dir_copy;
    }
  }

  wchar_t *appdata_path_w = NULL;
  if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL,
                           &appdata_path_w) == S_OK) {
    wchar_t full_appdata_path_w[APP_MAX_PATH_BUFFER];
    wcsncpy(full_appdata_path_w, appdata_path_w, APP_MAX_PATH_BUFFER - 1);
    full_appdata_path_w[APP_MAX_PATH_BUFFER - 1] = L'\0';
    CoTaskMemFree(appdata_path_w);
    PathAppendW(full_appdata_path_w, L"\\" APP_NAME);

    char *appdata_path_utf8 =
        arena ? (char *)mem_arena_alloc(arena, APP_MAX_PATH_BUFFER, false)
              : NULL;
    if (appdata_path_utf8) {
      if (WideCharToMultiByte(CP_UTF8, 0, full_appdata_path_w, -1,
                              appdata_path_utf8, APP_MAX_PATH_BUFFER, NULL,
                              NULL) > 0) {
        if (count < max_paths)
          paths[count++] = appdata_path_utf8;
      }
    }
  }

  wchar_t *programdata_path_w = NULL;
  if (SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL,
                           &programdata_path_w) == S_OK) {
    wchar_t full_programdata_path_w[APP_MAX_PATH_BUFFER];
    wcsncpy(full_programdata_path_w, programdata_path_w,
            APP_MAX_PATH_BUFFER - 1);
    full_programdata_path_w[APP_MAX_PATH_BUFFER - 1] = L'\0';
    CoTaskMemFree(programdata_path_w);
    PathAppendW(full_programdata_path_w, L"\\" APP_NAME);

    char *programdata_path_utf8 =
        arena ? (char *)mem_arena_alloc(arena, APP_MAX_PATH_BUFFER, false)
              : NULL;
    if (programdata_path_utf8) {
      if (WideCharToMultiByte(CP_UTF8, 0, full_programdata_path_w, -1,
                              programdata_path_utf8, APP_MAX_PATH_BUFFER, NULL,
                              NULL) > 0) {
        if (count < max_paths)
          paths[count++] = programdata_path_utf8;
      }
    }
  }
#else // POSIX
  if (count < max_paths)
    paths[count++] = ".";

  const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
  char *xdg_path =
      arena ? (char *)mem_arena_alloc(arena, APP_MAX_PATH_BUFFER, false) : NULL;
  if (xdg_path) {
    bool xdg_path_set = false;
    if (xdg_config_home && xdg_config_home[0] != '\0') {
      snprintf(xdg_path, APP_MAX_PATH_BUFFER, "%s/%s", xdg_config_home,
               APP_NAME);
      xdg_path_set = true;
    } else {
      const char *home_dir = getenv("HOME");
      if (home_dir) {
        snprintf(xdg_path, APP_MAX_PATH_BUFFER, "%s/.config/%s", home_dir,
                 APP_NAME);
        xdg_path_set = true;
      }
    }
    if (xdg_path_set && count < max_paths) {
      paths[count++] = xdg_path;
    }
  }

  if (count < max_paths)
    paths[count++] = "/etc/" APP_NAME;
  if (count < max_paths)
    paths[count++] = "/usr/local/etc/" APP_NAME;
#endif

  return count;
}

// --- CPU Feature Diagnostics ---

void platform_check_cpu_features(void) {
#if (defined(__GNUC__) || defined(__clang__)) &&                               \
    (defined(__x86_64__) || defined(__i386__))
  __builtin_cpu_init();

// 1. If this is an AVX2 build, directly check if the hardware supports it
#if defined(__AVX2__)
  if (!__builtin_cpu_supports("avx2")) {
    if (__builtin_cpu_supports("avx")) {
#ifdef _WIN32
      log_fatal("This binary uses AVX2 instructions, but your processor "
                "only supports AVX. Please download the AVX release build.");
#else
      log_fatal(
          "This binary uses AVX2 instructions, but your processor only "
          "supports AVX. Please recompile the software using the default "
          "Release "
          "build option without passing any custom CPU optimization flags.");
#endif
    } else {
#ifdef _WIN32
      log_fatal("This build of the application uses AVX2 instructions, which "
                "your processor does not support. Please rebuild the software "
                "yourself without AVX optimizations to run on this machine.");
#else
      log_fatal(
          "This build of the application uses AVX2 instructions, which "
          "your processor does not support. Please recompile the software "
          "using the default Release build option without passing any custom "
          "CPU optimization flags.");
#endif
    }
    exit(EXIT_FAILURE);
  }

// 2. If this is a standard AVX build, directly check hardware and warn if they
// could upgrade
#elif defined(__AVX__)
  if (!__builtin_cpu_supports("avx")) {
#ifdef _WIN32
    log_fatal(
        "This build of the application uses AVX instructions, which "
        "your processor does not support. Please rebuild the software yourself "
        "without AVX optimizations to run on this machine.");
#else
    log_fatal("This build of the application uses AVX instructions, "
              "which your processor does not support. Please recompile the "
              "software using the default Release build option without passing "
              "any custom CPU optimization flags.");
#endif
    exit(EXIT_FAILURE);
  }
  if (__builtin_cpu_supports("avx2")) {
#ifdef _WIN32
    log_warn("Your processor supports AVX2 instructions, but you are "
             "using the AVX build. Download the AVX2 release build for better "
             "performance.");
#else
    log_warn("Your processor supports AVX2 instructions, but this "
             "binary is only using AVX instructions. Consider recompiling the "
             "software using the default Release build option without passing "
             "any custom CPU optimization flags for better performance.");
#endif
  }

// 3. If someone compiled a BASELINE build (No AVX at all)
#else
  if (__builtin_cpu_supports("avx2")) {
#ifdef _WIN32
    log_warn("Your processor supports AVX2 instructions, but this "
             "binary is not using any AVX optimizations. Download the AVX2 "
             "release build for better performance.");
#else
    log_warn(
        "Your processor supports AVX2 instructions, but this binary is "
        "not using any AVX optimizations. Consider recompiling the software "
        "using the default Release build option without passing any custom CPU "
        "optimization flags for better performance.");
#endif
  } else if (__builtin_cpu_supports("avx")) {
#ifdef _WIN32
    log_warn("Your processor supports AVX instructions, but this "
             "binary is not using any AVX optimizations. Download the AVX "
             "release build for better performance.");
#else
    log_warn(
        "Your processor supports AVX instructions, but this binary is "
        "not using any AVX optimizations. Consider recompiling the software "
        "using the default Release build option without passing any custom CPU "
        "optimization flags for better performance.");
#endif
  }
#endif

  // ARM Architecture Diagnostics (NEON)
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__) &&     \
    !defined(__ARM_NEON)
#ifdef _WIN32
  log_warn("Your processor supports NEON instructions, but this binary is not "
           "using any NEON optimizations. Download the native ARM NEON "
           "release build for better performance.");
#else
  log_warn("Your processor supports NEON instructions, but this binary is not "
           "using any NEON optimizations. Consider recompiling the software "
           "using the default Release build option without passing any custom "
           "CPU optimization flags for better performance.");
#endif
#endif
}
