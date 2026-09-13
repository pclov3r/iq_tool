/**
 * @file dsp/dcblock.c
 * @brief DC offset blocking filter module.
 */

#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "module.h"
#include "module_registry.h"
#include "process_chain_types.h"
#include <liquid.h>
#include <math.h>
#include <stdlib.h>

// --- Default Configuration ---
// The cutoff frequency for the DC blocking high-pass filter.
#define DC_BLOCK_CUTOFF_HZ 50.0f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// === DSP Module Interface Implementation ===

static void *dcblock_initialize(ModuleContext *ctx, double input_rate,
                                double target_output_rate, double *out_rate) {
  (void)target_output_rate;
  *out_rate = input_rate;

  AppConfig *config = (AppConfig *)ctx->config;
  if (!config->dsp.dc_block.enable) {
    return NULL;
  }

  if (ctx->app->module.input_info.sample_rate <= 0.0) {
    log_error("Cannot initialize with invalid sample rate.");
    return NULL;
  }

  float normalized_alpha = (float)(2.0 * M_PI * DC_BLOCK_CUTOFF_HZ /
                                   ctx->app->module.input_info.sample_rate);

  if (normalized_alpha <= 0.0f) {
    log_error("Calculated normalized alpha (%.6f) is invalid.",
              normalized_alpha);
    return NULL;
  }
  if (normalized_alpha > 1.0f) {
    log_warn("Calculated normalized alpha (%.6f) is very large.",
             normalized_alpha);
  }

  iirfilt_crcf filter = iirfilt_crcf_create_dc_blocker(normalized_alpha);
  if (!filter) {
    log_fatal("Failed to create liquid-dsp DC block filter.");
    return NULL;
  }

  log_info("Enabled (High-pass 1st order)");
  return filter;
}

static SampleChunk *dcblock_process(void *state, SampleChunk *chunk) {
  iirfilt_crcf filter = (iirfilt_crcf)state;
  if (!filter)
    return chunk;

  if (chunk->stream_discontinuity_event) {
    log_debug("DC block filter reset due to stream discontinuity.");
    iirfilt_crcf_reset(filter);
  }

  iirfilt_crcf_execute_block(
      filter, (liquid_float_complex *)chunk->current_buffer, chunk->frames_read,
      (liquid_float_complex *)chunk->current_buffer);
  return chunk;
}

static void dcblock_cleanup(void *state) {
  if (state) {
    iirfilt_crcf_destroy((iirfilt_crcf)state);
  }
}

static void dcblock_reset_api(void *state) {
  if (state) {
    log_debug("DC block filter reset due to stream discontinuity.");
    iirfilt_crcf_reset((iirfilt_crcf)state);
  }
}

static bool s_enable_dc_block = false;

// clang-format off
static const struct argparse_option cli_options[] = {
        OPT_GROUP("DC Block Options"),
        OPT_BOOLEAN(0, "dc-block", &s_enable_dc_block, "(Optional) Enable DC offset removal (high-pass filter).", NULL, 0, 0),
};
// clang-format on

static const struct argparse_option *dsp_dcblock_get_cli_options(int *count) {
  *count = sizeof(cli_options) / sizeof(cli_options[0]);
  return cli_options;
}

static bool dsp_dcblock_validate_options(struct AppContext *app) {
  if (app && app->config) {
    ((AppConfig *)app->config)->dsp.dc_block.enable = s_enable_dc_block;
  }
  return true;
}

static bool dcblock_is_active(AppContext *app, const char *stage_tag) {
  (void)stage_tag;
  return ((AppConfig *)app->config)->dsp.dc_block.enable;
}

static const DspModuleInterface dsp_dcblock_api = {
    .name = "dc_block",
    .is_active = dcblock_is_active,
    .initialize = dcblock_initialize,
    .process = dcblock_process,
    .reset = dcblock_reset_api,
    .validate_options = dsp_dcblock_validate_options,
    .cleanup = dcblock_cleanup,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "dc_block",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_dcblock_api,
      .get_cli_options = dsp_dcblock_get_cli_options,
  };
  module_registry_add(&m);
}
