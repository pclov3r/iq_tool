/**
 * @file resampler.c
 * @brief Rational sample rate conversion and interpolation/decimation module.
 */

#include "app_context.h"
#include "log.h"
#include "mem_arena.h"
#include "module.h"
#include "module_registry.h"
#include "process_chain_types.h"
#include <liquid.h>
#include <math.h>

typedef struct ResamplerState {
  msresamp_crcf resamp;
  double target_rate;
} ResamplerState;

// === DSP Module Interface Implementation ===

static void *dsp_resampler_init(ModuleContext *ctx, double input_rate,
                                double target_output_rate, double *out_rate) {
  float resample_ratio = (float)(target_output_rate / input_rate);
  *out_rate = target_output_rate;

  log_info("Resampling: %.15g Hz -> %.15g Hz (Ratio: %.15g)", input_rate,
           target_output_rate, resample_ratio);

  msresamp_crcf q = msresamp_crcf_create(
      resample_ratio, ctx->config->dsp.filter.args.attenuation);
  if (!q) {
    log_fatal("Failed to create liquid-dsp resampler object.");
    return NULL;
  }

  ResamplerState *state = (ResamplerState *)mem_arena_alloc(
      &ctx->app->process_chain.setup_arena, sizeof(ResamplerState), true);
  if (!state) {
    msresamp_crcf_destroy(q);
    log_fatal("Failed to allocate Resampler state.");
    return NULL;
  }
  state->resamp = q;
  state->target_rate = target_output_rate;
  return state;
}

static SampleChunk *dsp_resampler_process(void *state, SampleChunk *chunk) {
  ResamplerState *resampler = (ResamplerState *)state;
  if (!resampler)
    return chunk;

  if (chunk->stream_discontinuity_event) {
    msresamp_crcf_reset(resampler->resamp);
  }

  unsigned int out_frames = 0;
  ComplexFloat *out_buffer = (chunk->current_buffer == chunk->ping_buffer)
                                 ? chunk->pong_buffer
                                 : chunk->ping_buffer;
  msresamp_crcf_execute(
      resampler->resamp, (liquid_float_complex *)chunk->current_buffer,
      chunk->frames_read, (liquid_float_complex *)out_buffer, &out_frames);
  chunk->frames_to_write = out_frames;
  chunk->current_buffer = out_buffer;
  chunk->sample_rate = resampler->target_rate;

  return chunk;
}

static void dsp_resampler_cleanup(void *state) {
  ResamplerState *resampler = (ResamplerState *)state;
  if (resampler && resampler->resamp) {
    msresamp_crcf_destroy(resampler->resamp);
    resampler->resamp = NULL;
  }
}

static void dsp_resampler_reset_api(void *state) {
  ResamplerState *resampler = (ResamplerState *)state;
  if (resampler && resampler->resamp) {
    msresamp_crcf_reset(resampler->resamp);
  }
}

static bool dsp_resampler_is_active(AppContext *app, const char *stage_tag) {
  (void)stage_tag;
  float resample_ratio = (float)(app->dsp.process_chain_sample_rate_hz /
                                 (double)app->module.input_info.sample_rate);
  AppConfig *config = (AppConfig *)app->config;
  return !(fabs(resample_ratio - 1.0f) < 1e-6 || config->dsp.raw_passthrough);
}

static size_t dsp_resampler_get_max_output_size(struct AppContext *app,
                                                size_t input_size) {
  float resample_ratio = (float)(app->dsp.process_chain_sample_rate_hz /
                                 (double)app->module.input_info.sample_rate);
  if (fabs(resample_ratio - 1.0f) < 1e-6) {
    return input_size;
  }
  return (size_t)ceil((double)(input_size + 32) * resample_ratio) + 64;
}

static size_t dsp_resampler_get_ideal_input_size(struct AppContext *app,
                                                 size_t target_output_size) {
  float resample_ratio = (float)(app->dsp.process_chain_sample_rate_hz /
                                 (double)app->module.input_info.sample_rate);
  if (resample_ratio > 1.0f) {
    return (size_t)(target_output_size / resample_ratio);
  }
  return target_output_size;
}

static const DspModuleInterface dsp_resampler_api = {
    .get_ideal_input_size = dsp_resampler_get_ideal_input_size,
    .name = "resampler",
    .is_active = dsp_resampler_is_active,
    .get_max_output_size = dsp_resampler_get_max_output_size,
    .initialize = dsp_resampler_init,
    .process = dsp_resampler_process,
    .reset = dsp_resampler_reset_api,
    .cleanup = dsp_resampler_cleanup,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "resampler",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_resampler_api,
  };
  module_registry_add(&m);
}
