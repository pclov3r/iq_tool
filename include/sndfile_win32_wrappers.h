/**
 * @file sndfile_win32_wrappers.h
 * @brief Win32 UTF-8 path wrapper for libsndfile.
 */

#ifndef SNDFILE_WIN32_WRAPPERS_H_
#define SNDFILE_WIN32_WRAPPERS_H_

#include <sndfile.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static inline SNDFILE *win32_sndfile_open_utf8(const char *utf8_path, int mode,
                                               SF_INFO *sound_file_info) {
  if (!utf8_path || !sound_file_info) {
    return NULL;
  }
  wchar_t wide_path[4096];
  if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide_path, 4096) <= 0) {
    return NULL;
  }
  return sf_wchar_open(wide_path, mode, sound_file_info);
}

#undef sf_open
#define sf_open(path, mode, sound_file_info)                                   \
  win32_sndfile_open_utf8((path), (mode), (sound_file_info))

#endif // _WIN32

#endif // SNDFILE_WIN32_WRAPPERS_H_
