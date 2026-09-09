/**
 * @file resampler.c
 */

#include "app_context.h"
#include "log.h"
#include "module.h"
#include "module_registry.h"
#include "process_chain_types.h"
#include <liquid.h>
#include <math.h>
#include <stdlib.h> // For exit()

typedef struct msresamp_crcf_s Resampler;

static Resampler *resampler_create(const AppConfig *config, AppContext *app,
                                   float resample_ratio) {
  (void)config; // config is not used here but kept for API consistency
  (void)app;

  // We use liquid-dsp's arbitrary resampler
  // liquid-dsp uses a polyphase filterbank for resampling.
  // We use the default parameters: As=60dB (stopband attenuation)
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
  float input_rate = (float)ctx->app->module.source_info.sample_rate;
  float output_rate = (float)ctx->app->dsp.process_chain_sample_rate_hz;
  float resample_ratio = output_rate / input_rate;

  log_info("Resampling: %.15g Hz -> %.15g Hz (Ratio: %.15g)", input_rate,
           output_rate, resample_ratio);

  return resampler_create(ctx->config, ctx->app, resample_ratio);
}

static SampleChunk *dsp_resampler_process(void *state, SampleChunk *chunk) {
  Resampler *resampler = (Resampler *)state;
  if (chunk->stream_discontinuity_event) {
    resampler_reset(resampler);
  }

  unsigned int out_frames = 0;
  ComplexFloat *out_buffer = (chunk->current_buffer == chunk->ping_buffer)
                              ? chunk->pong_buffer
                              : chunk->ping_buffer;
  resampler_execute(resampler, chunk->current_buffer, chunk->frames_read,
                    out_buffer, &out_frames);
  chunk->frames_to_write = out_frames;
  chunk->current_buffer = out_buffer;

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
  float r = (float)(app->dsp.process_chain_sample_rate_hz /
                    (double)app->module.source_info.sample_rate);
  AppConfig *config = (AppConfig *)app->config;
  return !(fabs(r - 1.0f) < 1e-6 || config->dsp.raw_passthrough);
}

static size_t dsp_resampler_get_max_output_size(struct AppContext *app,
                                                size_t input_size) {
  float r = (float)(app->dsp.process_chain_sample_rate_hz /
                    (double)app->module.source_info.sample_rate);
  if (fabs(r - 1.0f) < 1e-6) {
    return input_size;
  }
  return (size_t)ceil((double)(input_size + 32) * r) + 64;
}

static const DspModuleInterface dsp_resampler_api = {
    .name = "resampler",
    .is_active = dsp_resampler_is_active,
    .get_max_output_size = dsp_resampler_get_max_output_size,
    .initialize = dsp_resampler_init,
    .process = dsp_resampler_process,
    .reset = dsp_resampler_reset_api,
    .cleanup = dsp_resampler_cleanup};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "resampler",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_resampler_api,
  };
  module_registry_add(&m);
}
