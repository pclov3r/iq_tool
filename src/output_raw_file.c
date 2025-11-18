#include "output_raw_file.h"
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

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

// --- Private Data ---
typedef struct {
    FILE* handle;
    long long total_bytes_written;
} RawOutData;

// --- Helper Functions (migrated from output_writer.c) ---
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
static FILE* _secure_open_for_write(const char* out_path_utf8) {
    int fd = open(out_path_utf8, O_WRONLY | O_NOFOLLOW);
    if (fd >= 0) {
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
        if (!prompt_for_overwrite(out_path_utf8)) {
            close(fd);
            return NULL;
        }
        if (S_ISREG(stat_buf.st_mode)) {
            if (ftruncate(fd, 0) != 0) {
                log_fatal("Could not truncate file %s: %s", out_path_utf8, strerror(errno));
                close(fd);
                return NULL;
            }
        }
    } else if (errno == ENOENT) {
        fd = open(out_path_utf8, O_WRONLY | O_CREAT | O_NOFOLLOW, 0666);
        if (fd < 0) {
            log_fatal("Could not create file %s: %s", out_path_utf8, strerror(errno));
            return NULL;
        }
    } else {
        log_fatal("Error opening output file %s: %s", out_path_utf8, strerror(errno));
        return NULL;
    }
    FILE *file_stream = fdopen(fd, "wb");
    if (!file_stream) {
        log_fatal("Could not associate FILE stream with file descriptor: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    return file_stream;
}
#endif

// --- Module Implementation ---

static bool raw_out_initialize(ModuleContext* ctx) {
    const AppConfig* config = ctx->config;
    AppResources* resources = ctx->resources;

    RawOutData* data = (RawOutData*)mem_arena_alloc(&resources->setup_arena, sizeof(RawOutData), true);
    if (!data) {
        return false;
    }

    #ifdef _WIN32
    const char* out_path = config->effective_output_filename_utf8;
    data->handle = _secure_open_for_write(config, out_path);
    #else
    const char* out_path = config->effective_output_filename;
    data->handle = _secure_open_for_write(out_path);
    #endif

    if (!data->handle) {
        return false;
    }

    resources->output_module_private_data = data;
    return true;
}

static void* raw_out_run_writer(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    RawOutData* data = (RawOutData*)resources->output_module_private_data;

    unsigned char* local_write_buffer = (unsigned char*)resources->writer_local_buffer;
    if (!local_write_buffer) {
        handle_fatal_thread_error("Writer (raw-file): Local write buffer is NULL.", resources);
        return NULL;
    }

    while (true) {
        size_t bytes_read = ring_buffer_read(resources->writer_input_buffer, local_write_buffer, IO_OUTPUT_WRITER_CHUNK_SIZE);
        if (bytes_read == 0) {
            break; // End of stream
        }

        size_t written_bytes = fwrite(local_write_buffer, 1, bytes_read, data->handle);
        if (written_bytes > 0) {
            data->total_bytes_written += written_bytes;
        }

        if (written_bytes != bytes_read) {
            char error_buf[256];
            snprintf(error_buf, sizeof(error_buf), "Writer (raw-file): File write error: %s", strerror(errno));
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
    log_debug("Raw-file output writer thread is exiting.");
    return NULL;
}

static size_t raw_out_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    AppResources* resources = ctx->resources;
    RawOutData* data = (RawOutData*)resources->output_module_private_data;
    if (!data || !data->handle) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, data->handle);
    if (written > 0) {
        data->total_bytes_written += written;
    }
    return written;
}

static void raw_out_finalize_output(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    if (!resources->output_module_private_data) return;
    RawOutData* data = (RawOutData*)resources->output_module_private_data;

    if (data->handle) {
        fclose(data->handle);
        data->handle = NULL;
    }
    resources->final_output_size_bytes = data->total_bytes_written;
}

static void raw_out_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "RAW");
}

// --- The V-Table ---
static OutputModuleInterface raw_output_module_api = {
    .validate_options = NULL,
    .get_cli_options = NULL,
    .initialize = raw_out_initialize,
    .run_writer = raw_out_run_writer,
    .write_chunk = raw_out_write_chunk,
    .finalize_output = raw_out_finalize_output,
    .get_summary_info = raw_out_get_summary_info,
};

// --- Public Getter ---
OutputModuleInterface* get_raw_file_output_module_api(void) {
    return &raw_output_module_api;
}
