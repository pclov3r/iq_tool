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
        clear_stdin_buffer();
    }
    response = tolower(response);
    if (response != 'y') {
        if (response != '\n' && response != EOF) {
            log_debug("Operation cancelled by user.");
        }
        return false;
    }
    return true;
}

// --- Shared Implementation ---

bool wav_common_validate_options(AppConfig* config) {
    // This logic is identical for both WAV and RF64.
    if (config->output_format != CS16 && config->output_format != CU8) {
        log_fatal("Invalid sample format '%s' for WAV/RF64 container. Only 'cs16' and 'cu8' are supported.", config->output_sample_format_name);
        return false;
    }
    return true;
}

bool wav_common_initialize(ModuleContext* ctx, int sf_format_flag) {
    const AppConfig* config = ctx->config;
    AppResources* resources = ctx->resources;

    // Allocate the private data struct for this module instance.
    WavCommonData* data = (WavCommonData*)mem_arena_alloc(&resources->setup_arena, sizeof(WavCommonData), true);
    if (!data) return false;
    resources->output_module_private_data = data;

    // Use platform-specific UTF-8 path for messages.
    #ifdef _WIN32
    const char* out_path = config->effective_output_filename_utf8;
    #else
    const char* out_path = config->effective_output_filename;
    #endif

    // Check if the file exists and prompt for overwrite if necessary.
    bool file_exists = false;
    #ifdef _WIN32
    DWORD attrs = GetFileAttributesW(config->effective_output_filename_w);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) { log_fatal("Output path '%s' is a directory. Aborting.", out_path); return false; }
        file_exists = true;
    }
    #else
    struct stat stat_buf;
    if (lstat(out_path, &stat_buf) == 0) {
        file_exists = true;
        if (!S_ISREG(stat_buf.st_mode)) { log_fatal("Output path '%s' exists but is not a regular file. Aborting.", out_path); return false; }
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
    sfinfo.samplerate = (int)config->target_rate;
    sfinfo.channels = 2;
    sfinfo.format = sf_format_flag; // Use the specific format flag passed by the wrapper.

    switch (config->output_format) {
        case CS16: sfinfo.format |= SF_FORMAT_PCM_16; break;
        case CU8:  sfinfo.format |= SF_FORMAT_PCM_U8; break;
        default: return false; // Should be caught by validation.
    }

    // Verify that libsndfile supports this format combination.
    if (!sf_format_check(&sfinfo)) { log_fatal("libsndfile does not support the requested format (Rate: %d, Format: 0x%08X).", sfinfo.samplerate, sfinfo.format); return false; }

    // Open the file using the appropriate platform-specific function.
    #ifdef _WIN32
    data->handle = sf_wchar_open(config->effective_output_filename_w, SFM_WRITE, &sfinfo);
    #else
    data->handle = sf_open(out_path, SFM_WRITE, &sfinfo);
    #endif

    if (!data->handle) { log_fatal("Error opening output WAV file %s: %s", out_path, sf_strerror(NULL)); return false; }
    return true;
}

void* wav_common_run_writer(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    WavCommonData* data = (WavCommonData*)resources->output_module_private_data;

    unsigned char* local_buffer = (unsigned char*)resources->writer_local_buffer;
    if (!local_buffer) { handle_fatal_thread_error("WAV writer: Local buffer is NULL.", resources); return NULL; }

    // Main writer loop: read from ring buffer, write to file.
    while (true) {
        size_t bytes_read = ring_buffer_read(resources->writer_input_buffer, local_buffer, IO_OUTPUT_WRITER_CHUNK_SIZE);
        if (bytes_read == 0) break; // End of stream or shutdown signal.

        sf_count_t written = sf_write_raw(data->handle, local_buffer, bytes_read);
        if (written > 0) {
            data->total_bytes_written += written;
        }

        if ((size_t)written != bytes_read) {
            char error_buf[256];
            snprintf(error_buf, sizeof(error_buf), "WAV writer: File write error: %s", sf_strerror(data->handle));
            handle_fatal_thread_error(error_buf, resources);
            break;
        }

        // Update and invoke the progress callback.
        if (resources->progress_callback) {
            unsigned long long current_frames = data->total_bytes_written / resources->output_bytes_per_sample_pair;
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_output_frames = current_frames;
            pthread_mutex_unlock(&resources->progress_mutex);
            resources->progress_callback(current_frames, resources->expected_total_output_frames, data->total_bytes_written, resources->progress_callback_udata);
        }
    }
    log_debug("Common WAV writer thread is exiting.");
    return NULL;
}

size_t wav_common_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    AppResources* resources = ctx->resources;
    WavCommonData* data = (WavCommonData*)resources->output_module_private_data;
    if (!data || !data->handle || bytes_to_write == 0) return 0;
    sf_count_t written = sf_write_raw(data->handle, buffer, bytes_to_write);
    if (written > 0) data->total_bytes_written += written;
    return (size_t)written;
}

void wav_common_finalize_output(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    if (!resources->output_module_private_data) return;
    WavCommonData* data = (WavCommonData*)resources->output_module_private_data;
    if (data->handle) {
        sf_close(data->handle);
        data->handle = NULL;
    }
    resources->final_output_size_bytes = data->total_bytes_written;
}
