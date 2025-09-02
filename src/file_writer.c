#include "file_writer.h"
#include "app_context.h"
#include "memory_arena.h"
#include "log.h"
#include "platform.h"
#include "utils.h"
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#else
#include <io.h>
#include <fcntl.h>
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

#ifdef _WIN32
static FILE* _secure_open_for_write(const AppConfig* config, const char* out_path_utf8);
#else
static FILE* _secure_open_for_write(const char* out_path_utf8);
#endif


// --- Forward Declarations for RAW Writer Operations ---
static bool raw_open(FileWriterContext* ctx, const AppConfig* config, AppResources* resources, MemoryArena* arena);
static size_t raw_write(FileWriterContext* ctx, const void* buffer, size_t bytes_to_write);
static void raw_close(FileWriterContext* ctx);
static long long generic_get_total_bytes_written(const FileWriterContext* ctx);


// --- Forward Declarations for WAV Writer Operations ---
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

#ifdef _WIN32
static FILE* _secure_open_for_write(const AppConfig* config, const char* out_path_utf8) {
    HANDLE hFile = CreateFileW(config->effective_output_filename_w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) {
            if (!prompt_for_overwrite(out_path_utf8)) {
                return NULL;
            }
            hFile = CreateFileW(config->effective_output_filename_w, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE) {
                print_win_error("CreateFileW (overwrite)", GetLastError());
                return NULL;
            }
        } else {
            print_win_error("CreateFileW (create new)", GetLastError());
            return NULL;
        }
    }

    int fd = _open_osfhandle((intptr_t)hFile, _O_WRONLY | _O_BINARY);
    if (fd == -1) {
        CloseHandle(hFile);
        log_fatal("Failed to associate C file descriptor with Windows handle.");
        return NULL;
    }
    return _fdopen(fd, "wb");
}
#else
// --- START MODIFICATION: Fully secure POSIX open logic ---
static FILE* _secure_open_for_write(const char* out_path_utf8) {
    int fd = open(out_path_utf8, O_WRONLY | O_NOFOLLOW);
    if (fd >= 0) {
        // --- FILE EXISTS ---
        // We have a valid handle, now we verify it.
        struct stat stat_buf;
        if (fstat(fd, &stat_buf) != 0) {
            log_fatal("Could not fstat opened file %s: %s", out_path_utf8, strerror(errno));
            close(fd);
            return NULL;
        }
        if (!S_ISREG(stat_buf.st_mode) && !S_ISCHR(stat_buf.st_mode)) {
            log_fatal("Output path '%s' exists but is not a regular file. Aborting.", out_path_utf8);
            close(fd);
            return NULL;
        }

        // Now we can safely prompt the user. The handle is already open.
        if (!prompt_for_overwrite(out_path_utf8)) {
            close(fd);
            return NULL; // User cancelled
        }

        // User agreed. Truncate the file *if it's a regular file*. This is safe.
        // For special files like /dev/null, truncation is not needed and will fail.
        if (S_ISREG(stat_buf.st_mode)) {
            if (ftruncate(fd, 0) != 0) {
            log_fatal("Could not truncate file %s: %s", out_path_utf8, strerror(errno));
            close(fd);
            return NULL;
        }
        }
        // We can now use this fd.
    } else if (errno == ENOENT) {
        // --- FILE DOES NOT EXIST ---
        // We can now safely create it. O_NOFOLLOW is still good practice.
        fd = open(out_path_utf8, O_WRONLY | O_CREAT | O_NOFOLLOW, 0666);
        if (fd < 0) {
            log_fatal("Could not create file %s: %s", out_path_utf8, strerror(errno));
            return NULL;
        }
    } else {
        // Another error occurred during the initial open attempt.
        log_fatal("Error opening output file %s: %s", out_path_utf8, strerror(errno));
        return NULL;
    }

    // FIX: Check the return value of fdopen and close the descriptor on failure.
    FILE *file_stream = fdopen(fd, "wb");
    if (!file_stream) {
        log_fatal("Could not associate FILE stream with file descriptor: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    return file_stream;
}
// --- END MODIFICATION ---
#endif

static long long generic_get_total_bytes_written(const FileWriterContext* ctx) {
    return ctx->total_bytes_written;
}


// --- RAW Writer Implementation ---
static bool raw_open(FileWriterContext* ctx, const AppConfig* config, AppResources* resources, MemoryArena* arena) {
    (void)resources;

    if (config->output_to_stdout) {
        #ifdef _WIN32
        if (!set_stdout_binary()) return false;
        #endif
        RawWriterData* data = (RawWriterData*)mem_arena_alloc(arena, sizeof(RawWriterData), true);
        if (!data) {
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

    FILE* handle;
    #ifdef _WIN32
    handle = _secure_open_for_write(config, out_path);
    #else
    handle = _secure_open_for_write(out_path);
    #endif

    if (!handle) {
        return false;
    }

    RawWriterData* data = (RawWriterData*)mem_arena_alloc(arena, sizeof(RawWriterData), true);
    if (!data) {
        fclose(handle);
        return false;
    }
    data->handle = handle;
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
    ctx->private_data = NULL;
}


// --- WAV Writer Implementation ---
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

    int format;
    if (config->output_type == OUTPUT_TYPE_WAV) {
        format = SF_FORMAT_WAV;
    } else {
        format = SF_FORMAT_RF64;
    }

    switch (config->output_format) {
        case CS16: format |= SF_FORMAT_PCM_16; break;
        case CU8:  format |= SF_FORMAT_PCM_U8; break;
        default:
            log_fatal("Internal Error: Cannot create WAV file for invalid sample type '%s'.", config->sample_type_name);
            return false;
    }
    sfinfo.format = format;

    if (!sf_format_check(&sfinfo)) {
        log_fatal("libsndfile does not support the requested WAV format (Rate: %d, Format: 0x%08X).", sfinfo.samplerate, sfinfo.format);
        return false;
    }

    WavWriterData* data = (WavWriterData*)mem_arena_alloc(arena, sizeof(WavWriterData), true);
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
