#include "core/app_context.h"
#include <stdlib.h>
/**
 * @file resampler.c
 */

#include "core/app_context.h"
#include "core/module.h"
#include "core/process_chain_types.h"
#include "dsp/resampler.h"
#include "log.h"
#include <liquid.h>
#include <stdlib.h> // For exit()

typedef struct msresamp_crcf_s Resampler;

static Resampler *resampler_create(const AppConfig *config, AppContext *app,
                                   float resample_ratio) {
  (void)config; // config is not used here but kept for API consistency
  if (app->dsp.bypass_resampler) {
    return NULL; // No resampler needed in passthrough mode.
  }

  // We cast the liquid-dsp object to our opaque type.
  Resampler *resampler = (Resampler *)msresamp_crcf_create(
      resample_ratio, config->dsp.filter.args.attenuation);

  if (!resampler) {
    log_fatal("Error: Failed to create liquid-dsp resampler object.");
    return NULL;
  }
  return resampler;
}

static void resampler_destroy(Resampler *resampler) {
  if (resampler) {
    // We cast our opaque type back to the liquid-dsp type to destroy it.
    msresamp_crcf_destroy((msresamp_crcf)resampler);
  }
}

static void resampler_reset(Resampler *resampler) {
  if (resampler) {
    msresamp_crcf_reset((msresamp_crcf)resampler);
  }
}

static void resampler_execute(Resampler *resampler, ComplexFloat *input,
                              unsigned int num_input_frames,
                              ComplexFloat *output,
                              unsigned int *num_output_frames) {
  if (resampler) {
    msresamp_crcf_execute((msresamp_crcf)resampler,
                          (liquid_float_complex *)input, num_input_frames,
                          (liquid_float_complex *)output, num_output_frames);
  }
}

// === DSP Module Interface Implementation ===

static void *dsp_resampler_init(ModuleContext *ctx) {
  float resample_ratio = (float)ctx->app->dsp.process_chain_sample_rate_hz /
                         (float)ctx->app->module.source_info.sample_rate;
  return resampler_create(ctx->config, ctx->app, resample_ratio);
}

static SampleChunk *dsp_resampler_process(void *state, SampleChunk *chunk) {
  Resampler *resampler = (Resampler *)state;
  if (chunk->stream_discontinuity_event && resampler) {
    resampler_reset(resampler);
  }
  if (resampler) {
    unsigned int out_frames = 0;
    resampler_execute(resampler, chunk->pre_resample_buffer, chunk->frames_read,
                      chunk->post_resample_buffer, &out_frames);
    chunk->frames_to_write = out_frames;
  } else {
    chunk->frames_to_write = chunk->frames_read;
    if (chunk->pre_resample_buffer != chunk->post_resample_buffer) {
      for (unsigned int i = 0; i < chunk->frames_read; i++) {
        chunk->post_resample_buffer[i] = chunk->pre_resample_buffer[i];
      }
    }
  }
  return chunk;
}

static void dsp_resampler_cleanup(void *state) {
  resampler_destroy((Resampler *)state);
}

static void dsp_resampler_reset_api(void *state) {
  resampler_reset((Resampler *)state);
}

static bool dsp_resampler_is_active(AppContext *app, const char *stage_tag) {
  (void)stage_tag;
  return !app->dsp.bypass_resampler;
}

static const DspModuleInterface dsp_resampler_api = {
    .name = "resampler",
    .is_active = dsp_resampler_is_active,
    .initialize = dsp_resampler_init,
    .process = dsp_resampler_process,
    .reset = dsp_resampler_reset_api,
    .cleanup = dsp_resampler_cleanup};

const DspModuleInterface *dsp_resampler_get_api(void) {
  return &dsp_resampler_api;
}
