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
} StdoutData;

// --- Module Implementation ---

static bool stdout_out_initialize(ModuleContext* ctx) {
    AppContext* app = ctx->app;

    StdoutData* data = (StdoutData*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(StdoutData), true);
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

static void* stdout_out_run_writer(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    StdoutData* data = (StdoutData*)app->module.output_private_data;

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

static size_t stdout_out_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    AppContext* app = ctx->app;
    StdoutData* data = (StdoutData*)app->module.output_private_data;
    if (!data) return 0;

    size_t written = fwrite(buffer, 1, bytes_to_write, stdout);
    if (written > 0) {
        data->total_bytes_written += written;
    }
    return written;
}

static void stdout_out_finalize_output(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (!app->module.output_private_data) return;
    StdoutData* data = (StdoutData*)app->module.output_private_data;

    fflush(stdout);
    app->stats.final_output_size_bytes = data->total_bytes_written;
}

static void stdout_out_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "RAW Stream");
}

// --- The V-Table ---
static OutputModuleInterface stdout_output_module_api = {
    .validate_options = NULL,
    .get_cli_options = NULL,
    .initialize = stdout_out_initialize,
    .run_writer = stdout_out_run_writer,
    .write_chunk = stdout_out_write_chunk,
    .finalize_output = stdout_out_finalize_output,
    .get_summary_info = stdout_out_get_summary_info,
};

// --- Public Getter ---
OutputModuleInterface* get_stdout_output_module_api(void) {
    return &stdout_output_module_api;
}
