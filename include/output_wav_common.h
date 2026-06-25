/**
 * @file output_wav_common.h
 * @brief PRIVATE: Declares the shared interface for WAV and RF64 output modules.
 *
 * This header defines the common functions and data structures used by both
 * the standard WAV writer and the RF64 WAV writer to avoid code duplication.
 * It is not intended to be included by modules outside of the WAV writers.
 */

#ifndef OUTPUT_WAV_COMMON_H_
#define OUTPUT_WAV_COMMON_H_

#include "module.h"     // For ModuleContext
#include <sndfile.h>    // For SNDFILE*

// --- Forward Declaration ---
// This tells the compiler that a struct named AppConfig exists, allowing us
// to use pointers to it without needing to include the entire app_context.h.
struct AppConfig;

// --- Shared Data Structure ---

/**
 * @struct WavCommonContext
 * @brief Holds the private state for any libsndfile-based writer.
 */
typedef struct {
    SNDFILE* handle;
    long long total_bytes_written;
} WavCommonContext;

// --- Shared Function Declarations ---

/**
 * @brief Validates that the selected sample format is compatible with WAV output.
 */
bool output_wav_common_validate_options(struct AppContext* app);

/**
 * @brief The core initialization logic for opening a WAV or RF64 file.
 * @param context The module context.
 * @param sf_format_flag The specific libsndfile format flag (e.g., SF_FORMAT_WAV or SF_FORMAT_RF64).
 * @return true on success, false on failure.
 */
bool output_wav_common_initialize(ModuleContext* context, int sf_format_flag);

/**
 * @brief Writes a single chunk of raw data to the open file handle. (Used for passthrough mode).
 */
size_t output_wav_common_write_chunk(ModuleContext* context, const void* buffer, size_t bytes_to_write);

/**
 * @brief Finalizes the WAV/RF64 file by closing the handle and updating final size.
 */
void output_wav_common_cleanup(ModuleContext* context);

#endif // OUTPUT_WAV_COMMON_H_
