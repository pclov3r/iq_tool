/**
 * @file output_rawfile.c
 */

#include "output_rawfile.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "utilities.h"
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

// --- Helper Functions ---

static bool output_rawfile_validate_options(AppContext* app) {
    AppConfig* config = (AppConfig*)app->config;

    #ifdef _WIN32
    if (config->output.effective_path_utf8[0] == '\0') {
    #else
    if (!config->output.effective_path || config->output.effective_path[0] == '\0') {
    #endif
        log_error("Rawfile output requires a valid file path.");
        return false;
    }

    #ifdef _WIN32
    const char* out_path = config->output.effective_path_utf8;
    #else
    const char* out_path = config->output.effective_path;
    #endif

    if (!utility_verify_output_path(config, out_path)) {
        return false;
    }

    RawfileOutputContext* data = (RawfileOutputContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(RawfileOutputContext), true);
    if (!data) return false;
    app->module.output_private_data = data;

    #ifdef _WIN32
    data->handle = _wfopen(config->output.effective_path_w, L"wb");
    #else
    data->handle = fopen(out_path, "wb");
    #endif

    if (!data->handle) {
        log_error("Error opening output file %s: %s", out_path, strerror(errno));
        return false;
    }

    return true;
}

static bool output_rawfile_initialize(ModuleContext* context) {
    AppContext* app = context->app;
    RawfileOutputContext* data = (RawfileOutputContext*)app->module.output_private_data;
    if (!data || !data->handle) return false;
    return true;
}

static size_t output_rawfile_write_chunk(ModuleContext* context, const void* buffer, size_t bytes_to_write) {
    AppContext* app = context->app;
    RawfileOutputContext* data = (RawfileOutputContext*)app->module.output_private_data;
    if (!data || !data->handle) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, data->handle);
    if (written > 0) {
        data->total_bytes_written += written;
    }
    return written;
}

static void output_rawfile_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (!app->module.output_private_data) return;
    RawfileOutputContext* data = (RawfileOutputContext*)app->module.output_private_data;

    if (data->handle) {
        fclose(data->handle);
        data->handle = NULL;
    }
    app->stats.final_output_size_bytes = data->total_bytes_written;
}

static void output_rawfile_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
    add_summary_item(info, "Output Type", "RAW File");
}

static const struct argparse_option output_rawfile_cli_options[] = {
    OPT_GROUP("RAW File Output (rawfile)"),
    OPT_GROUP("    (No module-specific options)"),
};

const struct argparse_option* output_rawfile_get_cli_options(int* count) {
    *count = sizeof(output_rawfile_cli_options) / sizeof(output_rawfile_cli_options[0]);
    return output_rawfile_cli_options;
}

// --- The V-Table ---
static OutputModuleInterface s_output_rawfile_api = {
    .validate_options = output_rawfile_validate_options,
    .get_cli_options = output_rawfile_get_cli_options,
    .initialize = output_rawfile_initialize,
    .reset = NULL,
    .flush = NULL,
    .write_chunk = output_rawfile_write_chunk,
    .cleanup = output_rawfile_cleanup,
    .get_summary_info = output_rawfile_get_summary_info,
};

// --- Public Getter ---
OutputModuleInterface* output_rawfile_get_module_api(void) {
    return &s_output_rawfile_api;
}
