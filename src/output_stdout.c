/**
 * @file output_stdout.c
 */

#include "output_stdout.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "signal_handler.h"
#include "utilities.h"
#include "mem_arena.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

// --- Windows Specifics for Binary Mode ---
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// --- Private Data ---
typedef struct {
    long long total_bytes_written;
} StdoutContext;

// --- Module Implementation ---

static bool stdout_output_initialize(ModuleContext* context) {
    AppContext* app = context->app;

    StdoutContext* data = (StdoutContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(StdoutContext), true);
    if (!data) {
        return false;
    }

#ifdef _WIN32
    // Windows: stdout defaults to text mode (\n -> \r\n), which corrupts binary I/Q data.
    // We must forcefully set it to binary.
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        log_error("Writer (stdout): Failed to set binary mode: %s", strerror(errno));
        return false;
    }
#endif

    app->module.output_private_data = data;
    return true;
}

static size_t stdout_output_write_chunk(ModuleContext* context, const void* buffer, size_t bytes_to_write) {
    AppContext* app = context->app;
    StdoutContext* data = (StdoutContext*)app->module.output_private_data;
    if (!data) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, stdout);
    if (written > 0) {
        data->total_bytes_written += written;
    }
    return written;
}

static void stdout_output_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (!app->module.output_private_data) return;
    StdoutContext* data = (StdoutContext*)app->module.output_private_data;

    fflush(stdout);
    app->stats.final_output_size_bytes = data->total_bytes_written;
}

static void stdout_output_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
    add_summary_item(info, "Output Type", "stdout");
}

static const struct argparse_option stdout_output_cli_options[] = {
    OPT_GROUP("Standard Output (stdout)"),
    OPT_GROUP("    (No module-specific options)"),
};

const struct argparse_option* stdout_output_get_cli_options(int* count) {
    *count = sizeof(stdout_output_cli_options) / sizeof(stdout_output_cli_options[0]);
    return stdout_output_cli_options;
}

// --- The V-Table ---
static OutputModuleInterface s_stdout_output_api = {
    .validate_options = NULL,
    .get_cli_options = stdout_output_get_cli_options,
    .initialize = stdout_output_initialize,
    .reset = NULL,
    .flush = NULL,
    .write_chunk = stdout_output_write_chunk,
    .cleanup = stdout_output_cleanup,
    .get_summary_info = stdout_output_get_summary_info,
};

// --- Public Getter ---
OutputModuleInterface* output_stdout_get_module_api(void) {
    return &s_stdout_output_api;
}
