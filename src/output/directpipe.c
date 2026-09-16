/**
 * @file output/directpipe.c
 * @brief Direct pipe and subprocess streaming output module.
 */

#include "app_context.h"
#include "log.h"
#include "mem_arena.h"
#include "module.h"
#include "module_registry.h"
#include "platform.h"
#include "utilities.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

// Using 10 to guarantee no collision with default OS file descriptors (0-9)
#define TARGET_FD 10

typedef struct {
  long long total_bytes_written;
} DirectPipeContext;

static bool output_directpipe_initialize(ModuleContext *context) {
  DirectPipeContext *data = (DirectPipeContext *)mem_arena_alloc(
      &context->app->process_chain.setup_arena, sizeof(DirectPipeContext),
      true);
  if (!data)
    return false;

  /*
   * Check if the user routed FD 10 in their shell.
   * Prevents the app from silently throwing data into a void.
   */
  if (!platform_is_fd_valid(TARGET_FD)) {
    log_error("DirectPipe: File Descriptor %d is not open!", TARGET_FD);
    log_error("Please route it in your shell.");
    return false;
  }

  /* Force binary mode on Windows to prevent \n -> \r\n corruption (no-op on
   * POSIX) */
  platform_set_binary_mode_fd(TARGET_FD);

  context->app->module.output_private_data = data;
  return true;
}

static size_t output_directpipe_write_chunk(ModuleContext *context,
                                            const void *buffer,
                                            size_t bytes_to_write) {
  DirectPipeContext *data =
      (DirectPipeContext *)context->app->module.output_private_data;
  if (!data || bytes_to_write == 0)
    return 0;

  const char *output_bytes = (const char *)buffer;
  size_t bytes_left = bytes_to_write;

  /*
   * Directly writes to the kernel pipe.
   * Handles partial writes, signals, and full pipes seamlessly.
   */
  while (bytes_left > 0) {
    ssize_t written = platform_write(TARGET_FD, output_bytes, bytes_left);

    if (written > 0) {
      output_bytes += written;
      bytes_left -= written;
      data->total_bytes_written += written;
    } else if (written == 0) {
      /* Limit reached (disk full) or stream closed. Break to prevent infinite
       * loop. */
      break;
    } else { // written < 0
      if (errno == EINTR) {
        // OS interrupted by a signal. Try again.
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Pipe is temporarily full. Yield CPU briefly.
        platform_sleep_us(100);
        continue;
      }
      // Fatal error (EPIPE, EBADF). Break the loop.
      break;
    }
  }

  return (bytes_to_write - bytes_left);
}

static void output_directpipe_cleanup(ModuleContext *context) {
  DirectPipeContext *data =
      (DirectPipeContext *)context->app->module.output_private_data;
  if (data) {
    atomic_store_explicit(&context->app->stats.final_output_size_bytes,
                          (int_least64_t)data->total_bytes_written,
                          memory_order_relaxed);
  }
}

static void output_directpipe_get_summary_info(const ModuleContext *context,
                                               SummaryInfo *info) {
  (void)context;
  utility_add_summary_item(info, "Output Type", "directpipe");
}

// clang-format off
static const struct argparse_option directpipe_cli_options[] = {
        OPT_GROUP("Direct Pipe Output (directpipe)"),
        OPT_GROUP(" (Pure data pipe bypassing stdio\n" " Route with '10>' or '10> >(...)')"),
        OPT_GROUP(" (No module-specific options)"),
};
// clang-format on

static const struct argparse_option *
output_directpipe_get_cli_options(int *count) {
  *count = sizeof(directpipe_cli_options) / sizeof(directpipe_cli_options[0]);
  return directpipe_cli_options;
}

static void directpipe_get_pipeline_requirements(const struct AppConfig *config,
                                                 double *rate,
                                                 SampleFormat *format,
                                                 float *gain,
                                                 struct OutputAgcConfig *agc) {
  *rate = config->output_sample_rate.rate_hz;
  *format = config->output.sample_format;
  *gain = config->dsp.output_gain;
  *agc = config->dsp.output_agc;
}

static OutputModuleInterface s_directpipe_api = {
    .get_pipeline_requirements = directpipe_get_pipeline_requirements,
    .validate_options = NULL,
    .initialize = output_directpipe_initialize,
    .reset = NULL,
    .flush = NULL,
    .write_chunk = output_directpipe_write_chunk,
    .cleanup = output_directpipe_cleanup,
    .get_summary_info = output_directpipe_get_summary_info,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "directpipe",
      .type = MODULE_TYPE_OUTPUT,
      .payload = PAYLOAD_IQ,
      .api = (void *)&s_directpipe_api,
      .set_default_config = NULL,
      .get_cli_options = output_directpipe_get_cli_options,
      .requires_input_path = false,
      .requires_output_path = false,
  };
  module_registry_add(&m);
}
