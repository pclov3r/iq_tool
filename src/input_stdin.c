/**
 * @file input_stdin.c
 */

#include "input_stdin.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "utilities.h"
#include "sample_format_table.h"
#include "app_context.h"
#include "platform.h"
#include "input_common.h"
#include "mem_arena.h"
#include "argparse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define strcasecmp _stricmp
#endif

// --- CLI Config ---
static struct {
    double sample_rate_hz;
    float input_stdin_sample_rate_hz_arg;
    bool sample_rate_provided;
    char *format_str;
    bool format_provided;
} s_stdin_config;

// --- Private Module State ---
typedef struct {
    unsigned char* rx_buffer;
    size_t rx_buffer_size;
} StdinContext;

static const struct argparse_option input_stdin_cli_options[] = {
    OPT_GROUP("Standard Input (stdin)"),
    OPT_FLOAT(0, "stdin-input-sample-rate", &s_stdin_config.input_stdin_sample_rate_hz_arg, "(Required) The sample rate of the stdin stream in Hz.", NULL, 0, 0),
    OPT_STRING(0, "stdin-input-sample-format", &s_stdin_config.format_str, "(Required) The sample format of the stdin stream.", NULL, 0, 0),
};

const struct argparse_option* input_stdin_get_cli_options(int* count) {
    *count = sizeof(input_stdin_cli_options) / sizeof(input_stdin_cli_options[0]);
    return input_stdin_cli_options;
}

// --- Module Implementation ---

static bool input_stdin_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    (void)config;
    if (s_stdin_config.input_stdin_sample_rate_hz_arg > 0.0f) {
        s_stdin_config.sample_rate_hz = (double)s_stdin_config.input_stdin_sample_rate_hz_arg;
        s_stdin_config.sample_rate_provided = true;
    }

    if (!s_stdin_config.sample_rate_provided) {
        log_error("Missing required option --stdin-input-sample-rate <hz> for stdin input.");
        return false;
    }
    if (!s_stdin_config.format_str) {
        log_error("Missing required option --stdin-input-sample-format <format> for stdin input.");
        return false;
    }

    s_stdin_config.format_provided = true;
    return true;
}

static bool input_stdin_initialize(ModuleContext* context) {
    const AppConfig *config = context->config;
    (void)config;
    AppContext* app = context->app;

#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        log_fatal("Failed to set stdin to binary mode.");
        return false;
    }
#endif

    // 1. Allocate the context and the read buffer from the Memory Arena
    StdinContext* p = (StdinContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(StdinContext), true);
    if (!p) return false;

    // Use a 32KB buffer aligned to MEM_ARENA_ALIGNMENT
    p->rx_buffer_size = 32768;
    p->rx_buffer = (unsigned char*)mem_arena_alloc(&app->pipeline.setup_arena, p->rx_buffer_size, false);
    if (!p->rx_buffer) return false;

    app->module.input_private_data = p;

    // 2. Resolve format and metadata
    app->module.input_format = get_format_info_by_name(s_stdin_config.format_str) ? get_format_info_by_name(s_stdin_config.format_str)->format_enum : FORMAT_UNKNOWN;
    if (app->module.input_format == FORMAT_UNKNOWN) {
        log_error("Invalid stdin input format '%s'.", s_stdin_config.format_str);
        return false;
    }

    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(app->module.input_format);

    app->module.source_info.sample_rate = (int)s_stdin_config.sample_rate_hz;
    app->module.source_info.frames = -1;

    log_info("Reading raw I/Q data from standard input (stdin)...");
    return true;
}

static void* input_stdin_push_samples_to_queue(ModuleContext* context, QueueSamples queue_samples, void* pipeline_context) {
    AppContext* app = context->app;
    StdinContext* p = (StdinContext*)app->module.input_private_data;

    while (!is_shutdown_requested() && !app->stats.error_occurred) {
        // Read into the pre-allocated arena buffer
        size_t bytes_read = fread(p->rx_buffer, 1, p->rx_buffer_size, stdin);

        if (bytes_read == 0) {
            if (ferror(stdin)) {
                request_forceful_shutdown("Error reading from stdin pipe.", app);
            }
            break;
        }

        size_t frames_read = bytes_read / app->module.input_bytes_per_iq_sample;

        if (!queue_samples(pipeline_context, p->rx_buffer, frames_read, app->module.input_format)) {
            // Drop logic handled by pipeline
        }
    }

    log_info("End of stdin stream reached.");
    return NULL;
}

static size_t input_stdin_read_chunk(ModuleContext* context, void* buffer, size_t bytes_to_read) {
    size_t total_read = 0;
    unsigned char* ptr = (unsigned char*)buffer;

    while (total_read < bytes_to_read && !is_shutdown_requested()) {
        size_t bytes_read = fread(ptr + total_read, 1, bytes_to_read - total_read, stdin);
        if (bytes_read > 0) {
            total_read += bytes_read;
        } else {
            if (ferror(stdin)) {
                request_forceful_shutdown("Error reading from stdin pipe.", context->app);
            }
            break; // EOF or Error
        }
    }
    return total_read;
}

static void input_stdin_stop_sample_queue_push(ModuleContext* context) {
    (void)context;
}

static void input_stdin_cleanup(ModuleContext* context) {
    (void)context;
}

static void input_stdin_get_summary_info(const ModuleContext* context, InputSummaryInfo* info) {
    (void)context;
    utility_add_summary_item(info, "Input Source", "Standard Input (stdin)");
    utility_add_summary_item(info, "Input Format", "%s", s_stdin_config.format_str);
    utility_add_summary_item(info, "Input Sample Rate", "%.15g Hz", s_stdin_config.sample_rate_hz);
}

static InputModuleInterface s_input_stdin_api = {
    .initialize = input_stdin_initialize,
    .push_samples_to_queue = input_stdin_push_samples_to_queue,
    .read_chunk = input_stdin_read_chunk,
    .stop_sample_queue_push = input_stdin_stop_sample_queue_push,
    .cleanup = input_stdin_cleanup,
    .get_summary_info = input_stdin_get_summary_info,
    .validate_options = input_stdin_validate_options,
    .validate_generic_options = NULL,
    .pre_stream_iq_correction = NULL,
};

InputModuleInterface* input_stdin_get_module_api(void) {
    return &s_input_stdin_api;
}
