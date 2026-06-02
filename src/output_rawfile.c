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

static bool rawfile_output_initialize(ModuleContext* context) {
    const AppConfig* config = context->config;
    AppContext* app = context->app;

    RawfileOutputContext* data = (RawfileOutputContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(RawfileOutputContext), true);
    if (!data) return false;
    app->module.output_private_data = data;

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

    #ifdef _WIN32
    data->handle = _wfopen(config->output.effective_path_w, L"wb");
    #else
    data->handle = fopen(out_path, "wb");
    #endif

    if (!data->handle) {
        log_error("Error opening output file %s: %s", out_path, strerror(errno));
        return false;
    }

    app->module.output_private_data = data;
    return true;
}

static size_t rawfile_output_write_chunk(ModuleContext* context, const void* buffer, size_t bytes_to_write) {
    AppContext* app = context->app;
    RawfileOutputContext* data = (RawfileOutputContext*)app->module.output_private_data;
    if (!data || !data->handle) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, data->handle);
    if (written > 0) {
        data->total_bytes_written += written;
    }
    return written;
}

static void rawfile_output_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (!app->module.output_private_data) return;
    RawfileOutputContext* data = (RawfileOutputContext*)app->module.output_private_data;

    if (data->handle) {
        fclose(data->handle);
        data->handle = NULL;
    }
    app->stats.final_output_size_bytes = data->total_bytes_written;
}

static void rawfile_output_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
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
