#include "output_stdout.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "queue.h"
#include "signal_handler.h"
#include "utils.h"
#include "memory_arena.h"
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

static bool stdout_output_initialize(ModuleContext* ctx) {
    AppContext* app = ctx->app;

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

static void* stdout_output_run_writer(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    StdoutContext* data = (StdoutContext*)app->module.output_private_data;

    while (true) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(app->pipeline.writer_input_queue);
        if (!item) break; // Shutdown

        if (item->stream_discontinuity_event) {
            queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
            continue;
        }

        if (item->is_last_chunk) {
            queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
            break; // End of stream
        }

        size_t output_bytes_this_chunk = item->frames_to_write * app->module.output_bytes_per_sample_pair;
        if (output_bytes_this_chunk > 0) {
            size_t written_bytes = fwrite(item->final_output_data, 1, output_bytes_this_chunk, stdout);
            if (written_bytes > 0) {
                data->total_bytes_written += written_bytes;
            }
            if (written_bytes != output_bytes_this_chunk) {
                if (!is_shutdown_requested()) {
                    log_debug("Writer (stdout): write error, consumer likely closed pipe: %s", strerror(errno));
                    request_shutdown();
                }
                queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
                break;
            }
        }

        if (!queue_enqueue(app->pipeline.free_sample_chunk_queue, item)) {
            break; // Shutdown
        }
    }
    log_debug("Stdout output writer thread is exiting.");
    return NULL;
}

static size_t stdout_output_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    AppContext* app = ctx->app;
    StdoutContext* data = (StdoutContext*)app->module.output_private_data;
    if (!data) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, stdout);
    if (written > 0) {
        data->total_bytes_written += written;
    }
    return written;
}

static void stdout_output_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (!app->module.output_private_data) return;
    StdoutContext* data = (StdoutContext*)app->module.output_private_data;

    fflush(stdout);
    app->stats.final_output_size_bytes = data->total_bytes_written;
}

static void stdout_output_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "RAW Stream");
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
    .run_writer = stdout_output_run_writer,
    .write_chunk = stdout_output_write_chunk,
    .cleanup = stdout_output_cleanup,
    .get_summary_info = stdout_output_get_summary_info,
};

// --- Public Getter ---
OutputModuleInterface* get_stdout_output_module_api(void) {
    return &s_stdout_output_api;
}
