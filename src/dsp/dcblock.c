/**
 * @file dsp/dcblock.c
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

// Forward the C interface calls to the new module structure if needed, or
// implement it here directly.

static void *dc_block_create(AppConfig *config, AppContext *app) {
  if (!config->dsp.dc_block.enable) {
    return NULL;
  }

  if (app->module.source_info.sample_rate <= 0.0) {
    log_error("DC Block: Cannot initialize with invalid sample rate.");
    return NULL;
  }

  float normalized_alpha = (float)(2.0 * M_PI * DC_BLOCK_CUTOFF_HZ /
                                   app->module.source_info.sample_rate);

  if (normalized_alpha <= 0.0f) {
    log_error("DC Block: Calculated normalized alpha (%.6f) is invalid.",
              normalized_alpha);
    return NULL;
  }
  if (normalized_alpha > 1.0f) {
    log_warn("DC Block: Calculated normalized alpha (%.6f) is very large.",
             normalized_alpha);
  }

  void *dc_block_filter =
      (void *)iirfilt_crcf_create_dc_blocker(normalized_alpha);

  if (!dc_block_filter) {
    log_fatal("Failed to create liquid-dsp DC block filter.");
    return NULL;
  }

  log_info("DC Block enabled");
  return dc_block_filter;
}

static void dc_block_reset(void *state) {
  if (!state) {
    return;
  }
  log_debug("DC block filter reset due to stream discontinuity.");
  iirfilt_crcf_reset((iirfilt_crcf)state);
}

static void dc_block_apply(void *state, ComplexFloat *samples,
                           int num_samples) {
  if (!state) {
    return;
  }
  iirfilt_crcf_execute_block((iirfilt_crcf)state,
                             (liquid_float_complex *)samples, num_samples,
                             (liquid_float_complex *)samples);
}

static void dc_block_destroy(void *state) {
  if (state) {
    iirfilt_crcf_destroy((iirfilt_crcf)state);
  }
}

// === DSP Module Interface Implementation ===

static void *dcblock_initialize(ModuleContext *ctx) {
  log_info("DC Blocker: Enabled (High-pass 1st order)");
  return dc_block_create((AppConfig *)ctx->config, ctx->app);
}

static SampleChunk *dcblock_process(void *state, SampleChunk *chunk) {
  if (chunk->stream_discontinuity_event) {
    dc_block_reset(state);
  }
  dc_block_apply(state, chunk->current_buffer, chunk->frames_read);
  return chunk;
}

static void dcblock_cleanup(void *state) { dc_block_destroy(state); }

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

static void dcblock_reset_api(void *state) { dc_block_reset(state); }

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
    .get_cli_options = dsp_dcblock_get_cli_options,
    .cleanup = dcblock_cleanup};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "dc_block",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_dcblock_api,
  };
  module_registry_add(&m);
}
