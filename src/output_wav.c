#include "output_wav.h"
#include "module.h"
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
#include <sndfile.h>

#ifdef _WIN32
#else
#include <sys/stat.h>
#endif

// --- Private Data ---
// This struct holds the state for this specific module instance.
typedef struct {
    SNDFILE* handle;
    long long total_bytes_written;
} WavOutData;

// --- Helper Functions ---
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

// --- Module Implementation ---

static bool wav_validate_options(AppConfig* config) {
    if (config->output_format != CS16 && config->output_format != CU8) {
        log_fatal("Invalid sample format '%s' for WAV container. Only 'cs16' and 'cu8' are supported.", config->output_sample_format_name);
        return false;
    }
    return true;
}

static bool wav_initialize(ModuleContext* ctx) {
    const AppConfig* config = ctx->config;
    AppResources* resources = ctx->resources;

    WavOutData* data = (WavOutData*)mem_arena_alloc(&resources->setup_arena, sizeof(WavOutData), true);
    if (!data) return false;
    resources->output_module_private_data = data;

    #ifdef _WIN32
    const char* out_path = config->effective_output_filename_utf8;
    #else
    const char* out_path = config->effective_output_filename;
    #endif

    bool file_exists = false;
    #ifdef _WIN32
    DWORD attrs = GetFileAttributesW(config->effective_output_filename_w);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            log_fatal("Output path '%s' is a directory. Aborting.", out_path);
            return false;
        }
        file_exists = true;
    }
    #else
    struct stat stat_buf;
    if (lstat(out_path, &stat_buf) == 0) {
        file_exists = true;
        if (!S_ISREG(stat_buf.st_mode)) {
             log_fatal("Output path '%s' exists but is not a regular file. Aborting.", out_path);
             return false;
        }
    }
    #endif

    if (file_exists) {
        if (!prompt_for_overwrite(out_path)) {
            return false;
        }
    }

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)config->target_rate;
    sfinfo.channels = 2;
    sfinfo.format = SF_FORMAT_WAV; // This module ONLY uses the standard WAV format.

    switch (config->output_format) {
        case CS16: sfinfo.format |= SF_FORMAT_PCM_16; break;
        case CU8:  sfinfo.format |= SF_FORMAT_PCM_U8; break;
        default: return false; // Should be caught by validate_options
    }

    if (!sf_format_check(&sfinfo)) {
        log_fatal("libsndfile does not support the requested WAV format (Rate: %d, Format: 0x%08X).", sfinfo.samplerate, sfinfo.format);
        return false;
    }

    #ifdef _WIN32
    data->handle = sf_wchar_open(config->effective_output_filename_w, SFM_WRITE, &sfinfo);
    #else
    data->handle = sf_open(out_path, SFM_WRITE, &sfinfo);
    #endif

    if (!data->handle) {
        log_fatal("Error opening output WAV file %s: %s", out_path, sf_strerror(NULL));
        return false;
    }
    return true;
}

static void* wav_run_writer(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    WavOutData* data = (WavOutData*)resources->output_module_private_data;

    unsigned char* local_write_buffer = (unsigned char*)resources->writer_local_buffer;
    if (!local_write_buffer) {
        handle_fatal_thread_error("Writer (wav): Local write buffer is NULL.", resources);
        return NULL;
    }

    while (true) {
        size_t bytes_read = ring_buffer_read(resources->writer_input_buffer, local_write_buffer, IO_OUTPUT_WRITER_CHUNK_SIZE);
        if (bytes_read == 0) break;

        sf_count_t written_bytes = sf_write_raw(data->handle, local_write_buffer, bytes_read);
        if (written_bytes > 0) {
            data->total_bytes_written += written_bytes;
        }

        if ((size_t)written_bytes != bytes_read) {
            char error_buf[256];
            snprintf(error_buf, sizeof(error_buf), "Writer (wav): File write error: %s", sf_strerror(data->handle));
            handle_fatal_thread_error(error_buf, resources);
            break;
        }

        if (resources->progress_callback) {
            unsigned long long current_frames = data->total_bytes_written / resources->output_bytes_per_sample_pair;
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_output_frames = current_frames;
            pthread_mutex_unlock(&resources->progress_mutex);
            resources->progress_callback(current_frames, resources->expected_total_output_frames, data->total_bytes_written, resources->progress_callback_udata);
        }
    }
    log_debug("WAV writer thread is exiting.");
    return NULL;
}

static size_t wav_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    AppResources* resources = ctx->resources;
    WavOutData* data = (WavOutData*)resources->output_module_private_data;
    if (!data || !data->handle || bytes_to_write == 0) return 0;

    sf_count_t bytes_written = sf_write_raw(data->handle, buffer, bytes_to_write);

    if (bytes_written > 0) {
        data->total_bytes_written += bytes_written;
    }
    return (size_t)bytes_written;
}

static void wav_finalize_output(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    if (!resources->output_module_private_data) return;
    WavOutData* data = (WavOutData*)resources->output_module_private_data;
    if (data->handle) {
        sf_close(data->handle);
        data->handle = NULL;
    }
    resources->final_output_size_bytes = data->total_bytes_written;
}

static void wav_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "WAV");
}


// --- The V-Table ---
static OutputModuleInterface wav_module_api = {
    .validate_options = wav_validate_options,
    .get_cli_options = NULL,
    .initialize = wav_initialize,
    .run_writer = wav_run_writer,
    .write_chunk = wav_write_chunk,
    .finalize_output = wav_finalize_output,
    .get_summary_info = wav_get_summary_info,
};

// --- Public Getter ---
OutputModuleInterface* get_wav_output_module_api(void) {
    return &wav_module_api;
}
