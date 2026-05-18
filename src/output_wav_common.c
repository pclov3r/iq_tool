/**
 * @file output_wav_common.c
 * @brief Implements the shared logic for WAV and RF64 output modules.
 */

#include "output_wav_common.h"
#include <sndfile.h>
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "utils.h"
#include "signal_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#ifdef _WIN32
#else
#include <sys/stat.h>
#endif

// --- Private Helper ---
// This helper remains private to the common implementation.
static bool prompt_for_overwrite(const char* path_for_messages) {
    fprintf(stderr, "\nOutput file %s exists.\nOverwrite? (y/n): ", path_for_messages);
    int response = getchar();
    if (response != '\n' && response != EOF) {
        utils_clear_stdin();
    }
    response = tolower(response);
    if (response != 'y') {
        if (response != '\n' && response != EOF) {
            log_info("Operation cancelled by user.");
        }
        return false;
    }
    return true;
}

// --- Shared Implementation ---

bool wav_common_validate_options(AppConfig* config) {
    // This logic is identical for both WAV and RF64.
    if (config->output.sample_format != CS16 && config->output.sample_format != CU8) {
        log_error("Invalid sample format '%s' for WAV/RF64 container. Only 'cs16' and 'cu8' are supported.", config->output.sample_format_str);
        return false;
    }
    return true;
}

bool wav_common_initialize(ModuleContext* ctx, int sf_format_flag) {
    const AppConfig* config = ctx->config;
    AppContext* app = ctx->app;

    // Allocate the private data struct for this module instance.
    WavCommonContext* data = (WavCommonContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(WavCommonContext), true);
    if (!data) return false;
    app->module.output_private_data = data;

    // Use platform-specific UTF-8 path for messages.
    #ifdef _WIN32
    const char* out_path = config->output.effective_path_utf8;
    #else
    const char* out_path = config->output.effective_path;
    #endif

    // Check if the file exists and prompt for overwrite if necessary.
    bool file_exists = false;
    #ifdef _WIN32
    DWORD attrs = GetFileAttributesW(config->output.effective_path_w);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) { log_error("Output path '%s' is a directory. Aborting.", out_path); return false; }
        file_exists = true;
    }
    #else
    struct stat stat_buf;
    if (lstat(out_path, &stat_buf) == 0) {
        file_exists = true;
        if (!S_ISREG(stat_buf.st_mode)) { log_error("Output path '%s' exists but is not a regular file. Aborting.", out_path); return false; }
    }
    #endif

    if (file_exists) {
        if (!prompt_for_overwrite(out_path)) {
            return false;
        }
    }

    // Prepare the libsndfile info struct.
    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)config->output_sample_rate.rate_hz;
    sfinfo.channels = 2;
    sfinfo.format = sf_format_flag; // Use the specific format flag passed by the wrapper.

    switch (config->output.sample_format) {
        case CS16: sfinfo.format |= SF_FORMAT_PCM_16; break;
        case CU8:  sfinfo.format |= SF_FORMAT_PCM_U8; break;
        default: return false; // Should be caught by validation.
    }

    // Verify that libsndfile supports this format combination.
    if (!sf_format_check(&sfinfo)) { log_error("libsndfile does not support the requested format (Rate: %d, Format: 0x%08X).", sfinfo.samplerate, sfinfo.format); return false; }

    // Open the file using the appropriate platform-specific function.
    #ifdef _WIN32
    data->handle = sf_wchar_open(config->output.effective_path_w, SFM_WRITE, &sfinfo);
    #else
    data->handle = sf_open(out_path, SFM_WRITE, &sfinfo);
    #endif

    if (!data->handle) { log_error("Error opening output WAV file %s: %s", out_path, sf_strerror(NULL)); return false; }
    return true;
}



size_t wav_common_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    AppContext* app = ctx->app;
    WavCommonContext* data = (WavCommonContext*)app->module.output_private_data;
    if (!data || !data->handle || bytes_to_write == 0) return 0;
    sf_count_t written = sf_write_raw(data->handle, buffer, bytes_to_write);
    if (written > 0) data->total_bytes_written += written;
    return (size_t)written;
}

void wav_common_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (!app->module.output_private_data) return;
    WavCommonContext* data = (WavCommonContext*)app->module.output_private_data;
    if (data->handle) {
        sf_close(data->handle);
        data->handle = NULL;
    }
    app->stats.final_output_size_bytes = data->total_bytes_written;
}
