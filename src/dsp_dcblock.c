/**
 * @file dsp_dcblock.c
 */

#include "dsp_dcblock.h"
#include "app_context.h"
#include "constants.h"
#include "dc_block.h"
#include "log.h"
#include "module.h"
#include "process_chain_types.h"
#include <liquid.h>
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Forward the C interface calls to the new module structure if needed, or
// implement it here directly.

bool dc_block_create(AppConfig *config, AppContext *app) {
  if (!config->dsp.dc_block.enable) {
    app->dsp.dc_block.dc_block_filter = NULL;
    return true;
  }

  if (app->module.source_info.sample_rate <= 0.0) {
    log_error("DC Block: Cannot initialize with invalid sample rate.");
    return false;
  }

  float normalized_alpha = (float)(2.0 * M_PI * DC_BLOCK_CUTOFF_HZ /
                                   app->module.source_info.sample_rate);

  if (normalized_alpha <= 0.0f) {
    log_error("DC Block: Calculated normalized alpha (%.6f) is invalid.",
              normalized_alpha);
    return false;
  }
  if (normalized_alpha > 1.0f) {
    log_warn("DC Block: Calculated normalized alpha (%.6f) is very large.",
             normalized_alpha);
  }

  app->dsp.dc_block.dc_block_filter =
      (struct dc_blocker_s *)iirfilt_crcf_create_dc_blocker(normalized_alpha);

  if (!app->dsp.dc_block.dc_block_filter) {
    log_fatal("Failed to create liquid-dsp DC block filter.");
    return false;
  }

  log_info("DC Block enabled");
  return true;
}

void dc_block_reset(DspContext *dsp) {
  if (!dsp->config->dsp.dc_block.enable || !dsp->dc_block.dc_block_filter) {
    return;
  }
  log_debug("DC block filter reset due to stream discontinuity.");
  iirfilt_crcf_reset((iirfilt_crcf)dsp->dc_block.dc_block_filter);
}

void dc_block_apply(DspContext *dsp, ComplexFloat *samples, int num_samples) {
  if (!dsp->config->dsp.dc_block.enable || !dsp->dc_block.dc_block_filter) {
    return;
  }
  iirfilt_crcf_execute_block((iirfilt_crcf)dsp->dc_block.dc_block_filter,
                             (liquid_float_complex *)samples, num_samples,
                             (liquid_float_complex *)samples);
}

void dc_block_destroy(AppContext *app) {
  if (app->dsp.dc_block.dc_block_filter) {
    iirfilt_crcf_destroy((iirfilt_crcf)app->dsp.dc_block.dc_block_filter);
    app->dsp.dc_block.dc_block_filter = NULL;
  }
}

// === DSP Module Interface Implementation ===

static bool dcblock_initialize(ModuleContext *ctx) {
  return dc_block_create((AppConfig *)ctx->config, ctx->app);
}

static SampleChunk *dcblock_process(ModuleContext *ctx, SampleChunk *chunk) {
  if (chunk->stream_discontinuity_event) {
    dc_block_reset(&ctx->app->dsp);
  }
  dc_block_apply(&ctx->app->dsp, chunk->pre_resample_buffer,
                 chunk->frames_read);
  return chunk;
}

static void dcblock_cleanup(ModuleContext *ctx) { dc_block_destroy(ctx->app); }

static bool s_enable_dc_block = false;

static const struct argparse_option cli_options[] = {
    OPT_GROUP("DC Block Options"),
    OPT_BOOLEAN(0, "dc-block", &s_enable_dc_block,
                "(Optional) Enable DC offset removal (high-pass filter).", NULL,
                0, 0),
};

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

static void dcblock_reset_api(ModuleContext *ctx) {
  dc_block_reset(&ctx->app->dsp);
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
    .get_cli_options = dsp_dcblock_get_cli_options,
    .cleanup = dcblock_cleanup};

const DspModuleInterface *dsp_dcblock_get_api(void) { return &dsp_dcblock_api; }
