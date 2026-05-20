#include "output_directpipe.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "mem_arena.h"
#include "utils.h"
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define WRITE _write
#else
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#define WRITE write
#endif

// Using 10 to guarantee no collision with default OS file descriptors (0-9)
#define TARGET_FD 10

typedef struct {
    long long total_bytes_written;
} DirectPipeContext;

static bool directpipe_output_initialize(ModuleContext* ctx) {
    DirectPipeContext* data = (DirectPipeContext*)mem_arena_alloc(&ctx->app->pipeline.setup_arena, sizeof(DirectPipeContext), true);
    if (!data) return false;

#ifndef _WIN32
    /*
     * Check if the user routed FD 10 in their shell.
     * Prevents the app from silently throwing data into a void.
     */
    if (fcntl(TARGET_FD, F_GETFD) == -1 && errno == EBADF) {
        log_error("DirectPipe: File Descriptor %d is not open!", TARGET_FD);
        log_error("Please route it in your shell.");
        return false;
    }
#else
    /* Windows: Force binary mode to prevent \n -> \r\n corruption */
    _setmode(TARGET_FD, _O_BINARY);
#endif

    ctx->app->module.output_private_data = data;
    return true;
}

static size_t directpipe_output_write_chunk(ModuleContext* ctx, const void* buffer, size_t bytes_to_write) {
    DirectPipeContext* data = (DirectPipeContext*)ctx->app->module.output_private_data;
    if (!data || bytes_to_write == 0) return 0;

    const char* ptr = (const char*)buffer;
    size_t bytes_left = bytes_to_write;

    /*
     * Directly writes to the kernel pipe.
     * Handles partial writes, signals, and full pipes seamlessly.
     */
    while (bytes_left > 0) {
        ssize_t written = WRITE(TARGET_FD, ptr, bytes_left);

        if (written > 0) {
            ptr += written;
            bytes_left -= written;
            data->total_bytes_written += written;
        }
        else if (written == 0) {
            /* Limit reached (disk full) or stream closed. Break to prevent infinite loop. */
            break;
        }
        else { // written < 0
            if (errno == EINTR) {
                // OS interrupted by a signal. Try again.
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Pipe is temporarily full. Yield CPU briefly.
#ifndef _WIN32
                usleep(100);
#endif
                continue;
            }
            // Fatal error (EPIPE, EBADF). Break the loop.
            break;
        }
    }

    return (bytes_to_write - bytes_left);
}

static void directpipe_output_cleanup(ModuleContext* ctx) {
    DirectPipeContext* data = (DirectPipeContext*)ctx->app->module.output_private_data;
    if (data) {
        ctx->app->stats.final_output_size_bytes = data->total_bytes_written;
    }
}

static void directpipe_output_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "Direct OS Pipe (FD 10)");
}

static const struct argparse_option directpipe_cli_options[] = {
    OPT_GROUP("Direct Pipe Output (directpipe)"),
    OPT_GROUP("    (Pure data pipe bypassing stdio\n"
              "     Route with '10>' or '10> >(...)')"),
    OPT_GROUP("    (No module-specific options)"),
};

const struct argparse_option* directpipe_output_get_cli_options(int* count) {
    *count = sizeof(directpipe_cli_options) / sizeof(directpipe_cli_options[0]);
    return directpipe_cli_options;
}

static OutputModuleInterface s_directpipe_api = {
    .validate_options = NULL,
    .get_cli_options = directpipe_output_get_cli_options,
    .initialize = directpipe_output_initialize,
    .reset = NULL,
    .flush = NULL,
    .write_chunk = directpipe_output_write_chunk,
    .cleanup = directpipe_output_cleanup,
    .get_summary_info = directpipe_output_get_summary_info,
};

OutputModuleInterface* output_directpipe_get_module_api(void) {
    return &s_directpipe_api;
}
