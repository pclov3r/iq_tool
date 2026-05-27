#include "output_rawfile.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
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
} RawfileOutputContext;

// --- Helper Functions (migrated from output_writer.c) ---

#ifdef _WIN32
static FILE* _secure_open_for_write(const AppConfig* config, const char* out_path_utf8) {
    HANDLE hFile = CreateFileW(config->output.effective_path_w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) {
            if (!utils_prompt_for_overwrite(out_path_utf8)) {
                return NULL;
            }
            hFile = CreateFileW(config->output.effective_path_w, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
            log_error("Output path '%s' exists but is not a regular file. Aborting.", out_path_utf8);
            close(fd);
            return NULL;
        }
        if (!utils_prompt_for_overwrite(out_path_utf8)) {
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
        log_error("Error opening output file %s: %s", out_path_utf8, strerror(errno));
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

static bool rawfile_output_initialize(ModuleContext* ctx) {
    const AppConfig* config = ctx->config;
    AppContext* app = ctx->app;

    RawfileOutputContext* data = (RawfileOutputContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(RawfileOutputContext), true);
    if (!data) {
        return false;
    }

    #ifdef _WIN32
    const char* out_path = config->output.effective_path_utf8;
    data->handle = _secure_open_for_write(config, out_path);
    #else
    const char* out_path = config->output.effective_path;
    data->handle = _secure_open_for_write(out_path);
    #endif

    if (!data->handle) {
        return false;
    }

    app->module.output_private_data = data;
    return true;
}



static size_t rawfile_output_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    AppContext* app = ctx->app;
    RawfileOutputContext* data = (RawfileOutputContext*)app->module.output_private_data;
    if (!data || !data->handle) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, data->handle);
    if (written > 0) {
        data->total_bytes_written += written;
    }
    return written;
}

static void rawfile_output_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (!app->module.output_private_data) return;
    RawfileOutputContext* data = (RawfileOutputContext*)app->module.output_private_data;

    if (data->handle) {
        fclose(data->handle);
        data->handle = NULL;
    }
    app->stats.final_output_size_bytes = data->total_bytes_written;
}

static void rawfile_output_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "RAW File");
}

static const struct argparse_option rawfile_output_cli_options[] = {
    OPT_GROUP("RAW File Output (rawfile)"),
    OPT_GROUP("    (No module-specific options)"),
};

const struct argparse_option* rawfile_output_get_cli_options(int* count) {
    *count = sizeof(rawfile_output_cli_options) / sizeof(rawfile_output_cli_options[0]);
    return rawfile_output_cli_options;
}

// --- The V-Table ---
static OutputModuleInterface s_rawfile_output_api = {
    .validate_options = NULL,
    .get_cli_options = rawfile_output_get_cli_options,
    .initialize = rawfile_output_initialize,
    .reset = NULL,
    .flush = NULL,
    .write_chunk = rawfile_output_write_chunk,
    .cleanup = rawfile_output_cleanup,
    .get_summary_info = rawfile_output_get_summary_info,
};

// --- Public Getter ---
OutputModuleInterface* output_rawfile_get_module_api(void) {
    return &s_rawfile_output_api;
}
