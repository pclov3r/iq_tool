/**
 * @file input/stdin.c
 * @brief Input source module for standard input (stdin) pipe.
 */

#include "app_context.h"
#include "argparse.h"
#include "config/constants.h"

#include "log.h"
#include "mem_arena.h"
#include "module_registry.h"
#include "platform.h"
#include "sample_format_table.h"
#include "signal_handler.h"
#include "utilities.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Default Configuration ---
#define STDIN_DEMOD_AUDIO_BUFFER_SIZE (128 * 1024)

// --- CLI Config ---
static struct {
  float input_stdin_sample_rate_hz_arg;
  char *format_str;
} s_stdin_config;

// --- Private Module State ---
typedef struct {
  unsigned char *rx_buffer;
  size_t rx_buffer_size;
} StdinContext;

// clang-format off
static const struct argparse_option input_stdin_cli_options[] = {
        OPT_GROUP("Standard Input (stdin)"),
        OPT_FLOAT(0, "stdin-input-sample-rate", &s_stdin_config.input_stdin_sample_rate_hz_arg, "(Required) The sample rate of the stdin stream in Hz.", NULL, 0, 0),
        OPT_STRING(0, "stdin-input-sample-format", &s_stdin_config.format_str, "(Required) The sample format of the stdin stream.", NULL, 0, 0),
};
// clang-format on

static const struct argparse_option *input_stdin_get_cli_options(int *count) {
  *count = sizeof(input_stdin_cli_options) / sizeof(input_stdin_cli_options[0]);
  return input_stdin_cli_options;
}

// --- Module Implementation ---

static bool input_stdin_validate_options(AppContext *app) {
  AppConfig *config = app ? (AppConfig *)app->config : NULL;
  if (s_stdin_config.input_stdin_sample_rate_hz_arg <= 0.0f) {
    log_error("Missing required option --stdin-input-sample-rate <hz> for "
              "stdin input.");
    return false;
  }
  config->input_sample_rate_hz =
      (double)s_stdin_config.input_stdin_sample_rate_hz_arg;

  if (!s_stdin_config.format_str) {
    log_error("Missing required option --stdin-input-sample-format <format> "
              "for stdin input.");
    return false;
  }

  return true;
}

static bool input_stdin_initialize(ModuleContext *context) {
  AppContext *app = context->app;

  if (!platform_set_binary_mode(stdin)) {
    log_fatal("Failed to set stdin to binary mode.");
    return false;
  }

  // 1. Allocate the context and the read buffer from the Memory Arena
  StdinContext *stdin_context = (StdinContext *)mem_arena_alloc(
      &app->process_chain.setup_arena, sizeof(StdinContext), true);
  if (!stdin_context)
    return false;

  // Use a 32KB buffer aligned to MEM_ARENA_ALIGNMENT
  stdin_context->rx_buffer_size = 32768;
  stdin_context->rx_buffer = (unsigned char *)mem_arena_alloc(
      &app->process_chain.setup_arena, stdin_context->rx_buffer_size, false);
  if (!stdin_context->rx_buffer)
    return false;

  app->module.input_private_data = stdin_context;

  // 2. Resolve format and metadata
  app->module.input_format =
      get_format_info_by_name(s_stdin_config.format_str)
          ? get_format_info_by_name(s_stdin_config.format_str)->format_enum
          : FORMAT_UNKNOWN;
  if (app->module.input_format == FORMAT_UNKNOWN) {
    log_error("Invalid stdin input format '%s'.", s_stdin_config.format_str);
    return false;
  }

  app->module.input_bytes_per_iq_sample =
      get_bytes_per_iq_sample(app->module.input_format);

  app->module.input_info.frames = -1;

  log_info("Reading raw I/Q data from standard input (stdin)...");
  return true;
}

static void *input_stdin_push_samples_to_queue(ModuleContext *context,
                                               QueueSamples queue_samples,
                                               void *process_chain_context) {
  AppContext *app = context->app;
  StdinContext *p = (StdinContext *)app->module.input_private_data;

  while (!is_shutdown_requested() && !input_has_error(app)) {
    // Read into the pre-allocated arena buffer
    size_t bytes_read = fread(p->rx_buffer, 1, p->rx_buffer_size, stdin);

    if (bytes_read == 0) {
      if (ferror(stdin)) {
        request_forceful_shutdown("Error reading from stdin pipe.", app);
      }
      break;
    }

    size_t frames_read = bytes_read / app->module.input_bytes_per_iq_sample;

    if (!queue_samples(process_chain_context, p->rx_buffer, frames_read,
                       app->module.input_format)) {
      // Drop logic handled by process_chain
    }
  }

  log_info("End of stdin stream reached.");
  return NULL;
}

static size_t input_stdin_read_chunk(ModuleContext *context, void *buffer,
                                     size_t bytes_to_read) {
  size_t total_read = 0;
  unsigned char *ptr = (unsigned char *)buffer;

  while (total_read < bytes_to_read && !is_shutdown_requested()) {
    size_t bytes_read =
        fread(ptr + total_read, 1, bytes_to_read - total_read, stdin);
    if (bytes_read > 0) {
      total_read += bytes_read;
    } else {
      if (ferror(stdin)) {
        request_forceful_shutdown("Error reading from stdin pipe.",
                                  context->app);
      }
      break; // EOF or Error
    }
  }
  return total_read;
}

static void input_stdin_stop_sample_queue_push(ModuleContext *context) {
  (void)context;
}

static void input_stdin_cleanup(ModuleContext *context) { (void)context; }

static void input_stdin_get_summary_info(const ModuleContext *context,
                                         SummaryInfo *info) {
  const AppConfig *config = context->config;
  utility_add_summary_item(info, "Input Source", "Standard Input (stdin)");
  utility_add_summary_item(info, "Input Format", "%s",
                           s_stdin_config.format_str);
  utility_add_summary_item(info, "Input Sample Rate", "%.15g Hz",
                           config->input_sample_rate_hz);
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

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "stdin",
      .type = MODULE_TYPE_INPUT,
      .default_filter_attenuation_db = 0.0f,
      .api = (void *)&s_input_stdin_api,
      .process_chain_mode = PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL,
      .set_default_config = NULL,
      .get_cli_options = input_stdin_get_cli_options,
      .requires_input_path = false,
      .requires_output_path = false,
      .default_demod_audio_buffer_size = STDIN_DEMOD_AUDIO_BUFFER_SIZE,
  };
  module_registry_add(&m);
}
