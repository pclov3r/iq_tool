/**
 * @file output/rawfile.c
 * @brief Raw binary sample file output writer module.
 */

#include "app_context.h"
#include "log.h"
#include "module.h"
#include "module_registry.h"
#include "platform.h"
#include "utilities.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Private Data ---
typedef struct {
  FILE *handle;
  long long total_bytes_written;
} RawfileOutputContext;

// --- Helper Functions ---

static bool output_rawfile_validate_options(AppContext *app) {
  AppConfig *config = (AppConfig *)app->config;

  if (!config->output.resolved_path ||
      config->output.resolved_path[0] == '\0') {
    log_error("Rawfile output requires a valid file path.");
    return false;
  }

  const char *path = config->output.resolved_path;

  if (!utility_verify_output_path(config, path)) {
    return false;
  }

  RawfileOutputContext *data = (RawfileOutputContext *)mem_arena_alloc(
      &app->process_chain.setup_arena, sizeof(RawfileOutputContext), true);
  if (!data)
    return false;
  app->module.output_private_data = data;

  data->handle = platform_file_open_write(path);

  if (!data->handle) {
    log_error("Error opening output file %s: %s", path, strerror(errno));
    return false;
  }

  return true;
}

static bool output_rawfile_initialize(ModuleContext *context) {
  AppContext *app = context->app;
  RawfileOutputContext *data =
      (RawfileOutputContext *)app->module.output_private_data;
  if (!data || !data->handle)
    return false;
  return true;
}

static size_t output_rawfile_write_chunk(ModuleContext *context,
                                         const void *buffer,
                                         size_t bytes_to_write) {
  AppContext *app = context->app;
  RawfileOutputContext *data =
      (RawfileOutputContext *)app->module.output_private_data;
  if (!data || !data->handle)
    return 0;

  size_t written = fwrite(buffer, 1, bytes_to_write, data->handle);
  if (written > 0) {
    data->total_bytes_written += written;
  }
  return written;
}

static void output_rawfile_cleanup(ModuleContext *context) {
  AppContext *app = context->app;
  if (!app->module.output_private_data)
    return;
  RawfileOutputContext *data =
      (RawfileOutputContext *)app->module.output_private_data;

  if (data->handle) {
    fclose(data->handle);
    data->handle = NULL;
  }
  atomic_store_explicit(&app->stats.final_output_size_bytes,
                        (int_least64_t)data->total_bytes_written,
                        memory_order_relaxed);
}

static void output_rawfile_get_summary_info(const ModuleContext *context,
                                            OutputSummaryInfo *info) {
  (void)context;
  utility_add_summary_item(info, "Output Type", "RAW File");
}

// clang-format off
static const struct argparse_option output_rawfile_cli_options[] = {
        OPT_GROUP("RAW File Output (rawfile)"),
        OPT_GROUP(" (No module-specific options)"),
};
// clang-format on

static const struct argparse_option *
output_rawfile_get_cli_options(int *count) {
  *count = sizeof(output_rawfile_cli_options) /
           sizeof(output_rawfile_cli_options[0]);
  return output_rawfile_cli_options;
}

// --- The V-Table ---

static void output_rawfile_get_pipeline_requirements(
    const struct AppConfig *config, double *rate, SampleFormat *format,
    float *gain, struct OutputAgcConfig *agc) {
  *rate = config->output_sample_rate.rate_hz;
  *format = config->output.sample_format;
  *gain = config->dsp.output_gain;
  *agc = config->dsp.output_agc;
}

static OutputModuleInterface s_output_rawfile_api = {
    .get_pipeline_requirements = output_rawfile_get_pipeline_requirements,
    .validate_options = output_rawfile_validate_options,
    .initialize = output_rawfile_initialize,
    .reset = NULL,
    .flush = NULL,
    .write_chunk = output_rawfile_write_chunk,
    .cleanup = output_rawfile_cleanup,
    .get_summary_info = output_rawfile_get_summary_info,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "rawfile",
      .type = MODULE_TYPE_OUTPUT,
      .payload = PAYLOAD_IQ,
      .api = (void *)&s_output_rawfile_api,
      .set_default_config = NULL,
      .get_cli_options = output_rawfile_get_cli_options,
      .requires_input_path = false,
      .requires_output_path = true,
  };
  module_registry_add(&m);
}
