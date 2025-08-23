// file_writer.c

#include "file_writer.h"
#include "log.h"
#include "platform.h"
#include "utils.h"
// MODIFIED: Include memory_arena.h for mem_arena_alloc
#include "memory_arena.h"
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#ifndef _WIN32
#include <unistd.h> // For access()
#endif

// --- Private Data Structs ---
typedef struct {
    FILE* handle;
} RawWriterData;

typedef struct {
    SNDFILE* handle;
} WavWriterData;


// --- Forward Declarations for Static Helper Functions ---
static bool prompt_for_overwrite(const char* path_for_messages);


// --- Forward Declarations for RAW Writer Operations ---
// MODIFIED: Signature updated to accept MemoryArena
static bool raw_open(FileWriterContext* ctx, const AppConfig* config, AppResources* resources, MemoryArena* arena);
static size_t raw_write(FileWriterContext* ctx, const void* buffer, size_t bytes_to_write);
static void raw_close(FileWriterContext* ctx);
static long long generic_get_total_bytes_written(const FileWriterContext* ctx);


// --- Forward Declarations for WAV Writer Operations ---
// MODIFIED: Signature updated to accept MemoryArena
static bool wav_open(FileWriterContext* ctx, const AppConfig* config, AppResources* resources, MemoryArena* arena);
static size_t wav_write(FileWriterContext* ctx, const void* buffer, size_t bytes_to_write);
static void wav_close(FileWriterContext* ctx);


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

    fprintf(stderr, "\n");

    return true;
}

static long long generic_get_total_bytes_written(const FileWriterContext* ctx) {
    return ctx->total_bytes_written;
}


// --- RAW Writer Implementation ---
// MODIFIED: Signature updated to accept MemoryArena
static bool raw_open(FileWriterContext* ctx, const AppConfig* config, AppResources* resources, MemoryArena* arena) {
    (void)resources;

    if (config->output_to_stdout) {
        #ifdef _WIN32
        if (!set_stdout_binary()) return false;
        #endif
        // MODIFIED: Allocate from arena instead of malloc
        RawWriterData* data = (RawWriterData*)mem_arena_alloc(arena, sizeof(RawWriterData));
        if (!data) {
            // mem_arena_alloc logs the error, no need to log again
            return false;
        }
        data->handle = stdout;
        ctx->private_data = data;
        return true;
    }

#ifdef _WIN32
    const char* out_path = config->effective_output_filename_utf8;
#else
    const char* out_path = config->effective_output_filename;
#endif

    bool file_exists = false;
    #ifdef _WIN32
    DWORD attrs = GetFileAttributesW(config->effective_output_filename_w);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        file_exists = true;
    }
    #else
    if (access(out_path, F_OK) == 0) {
        file_exists = true;
    }
    #endif

    if (file_exists) {
        if (!prompt_for_overwrite(out_path)) {
            return false;
        }
    }

    // MODIFIED: Allocate from arena instead of malloc
    RawWriterData* data = (RawWriterData*)mem_arena_alloc(arena, sizeof(RawWriterData));
    if (!data) {
        return false;
    }

    #ifdef _WIN32
    data->handle = _wfopen(config->effective_output_filename_w, L"wb");
    #else
    data->handle = fopen(out_path, "wb");
    #endif

    if (!data->handle) {
        log_fatal("Error opening output file %s: %s", out_path, strerror(errno));
        // REMOVED: free(data); - Memory is now managed by the arena
        return false;
    }

    ctx->private_data = data;
    return true;
}

static size_t raw_write(FileWriterContext* ctx, const void* buffer, size_t bytes_to_write) {
    RawWriterData* data = (RawWriterData*)ctx->private_data;
    if (!data || !data->handle) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, data->handle);
    if (written > 0) {
        ctx->total_bytes_written += written;
    }
    return written;
}

static void raw_close(FileWriterContext* ctx) {
    if (!ctx || !ctx->private_data) return;
    RawWriterData* data = (RawWriterData*)ctx->private_data;
    if (data->handle && data->handle != stdout) {
        fclose(data->handle);
    }
    // REMOVED: free(data); - Memory is now managed by the arena
    ctx->private_data = NULL;
}


// --- WAV Writer Implementation ---
// MODIFIED: Signature updated to accept MemoryArena
static bool wav_open(FileWriterContext* ctx, const AppConfig* config, AppResources* resources, MemoryArena* arena) {
    (void)resources;

#ifdef _WIN32
    const char* out_path = config->effective_output_filename_utf8;
#else
    const char* out_path = config->effective_output_filename;
#endif

    bool file_exists = false;
    #ifdef _WIN32
    DWORD attrs = GetFileAttributesW(config->effective_output_filename_w);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        file_exists = true;
    }
    #else
    if (access(out_path, F_OK) == 0) {
        file_exists = true;
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

    int format;
    if (config->output_type == OUTPUT_TYPE_WAV) {
        format = SF_FORMAT_WAV;
    } else { // This handles OUTPUT_TYPE_WAV_RF64
        format = SF_FORMAT_RF64;
    }

    switch (config->output_format) {
        case CS16: format |= SF_FORMAT_PCM_16; break;
        case CU8:  format |= SF_FORMAT_PCM_U8; break;
        // CS8 is intentionally not handled here as it's invalid for WAV
        // and already validated in cli.c
        default:
            log_fatal("Internal Error: Cannot create WAV file for invalid sample type '%s'.", config->sample_type_name);
            return false;
    }
    sfinfo.format = format;

    if (!sf_format_check(&sfinfo)) {
        log_fatal("libsndfile does not support the requested WAV format (Rate: %d, Format: 0x%08X).", sfinfo.samplerate, sfinfo.format);
        return false;
    }

    // MODIFIED: Allocate from arena instead of malloc
    WavWriterData* data = (WavWriterData*)mem_arena_alloc(arena, sizeof(WavWriterData));
    if (!data) {
        return false;
    }

    #ifdef _WIN32
    data->handle = sf_wchar_open(config->effective_output_filename_w, SFM_WRITE, &sfinfo);
    #else
    data->handle = sf_open(out_path, SFM_WRITE, &sfinfo);
    #endif

    if (!data->handle) {
        log_fatal("Error opening output WAV file %s: %s", out_path, sf_strerror(NULL));
        // REMOVED: free(data); - Memory is now managed by the arena
        return false;
    }

    ctx->private_data = data;
    return true;
}

static size_t wav_write(FileWriterContext* ctx, const void* buffer, size_t bytes_to_write) {
    WavWriterData* data = (WavWriterData*)ctx->private_data;
    if (!data || !data->handle || bytes_to_write == 0) return 0;

    sf_count_t bytes_written = sf_write_raw(data->handle, buffer, bytes_to_write);

    if (bytes_written > 0) {
        ctx->total_bytes_written += bytes_written;
    }
    return (size_t)bytes_written;
}

static void wav_close(FileWriterContext* ctx) {
    if (!ctx || !ctx->private_data) return;
    WavWriterData* data = (WavWriterData*)ctx->private_data;
    if (data->handle) {
        sf_close(data->handle);
    }
    // REMOVED: free(data); - Memory is now managed by the arena
    ctx->private_data = NULL;
}


// --- Public Factory Function ---
bool file_writer_init(FileWriterContext* ctx, const AppConfig* config) {
    memset(ctx, 0, sizeof(FileWriterContext));

    switch (config->output_type) {
        case OUTPUT_TYPE_RAW:
            ctx->ops.open = raw_open;
            ctx->ops.write = raw_write;
            ctx->ops.close = raw_close;
            ctx->ops.get_total_bytes_written = generic_get_total_bytes_written;
            break;
        case OUTPUT_TYPE_WAV:
        case OUTPUT_TYPE_WAV_RF64:
            ctx->ops.open = wav_open;
            ctx->ops.write = wav_write;
            ctx->ops.close = wav_close;
            ctx->ops.get_total_bytes_written = generic_get_total_bytes_written;
            break;
        default:
            log_fatal("Internal Error: Unknown output type specified.");
            return false;
    }
    return true;
}
