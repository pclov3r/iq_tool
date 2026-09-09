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
#include "mem_arena.h"

typedef struct {
  struct msresamp_crcf_s *q;
  double target_rate;
} Resampler;

static Resampler *resampler_create(const AppConfig *config, AppContext *app,
                                   MemoryArena *arena, float resample_ratio) {
  // We use liquid-dsp's arbitrary resampler (polyphase filterbank).
  // We cast the liquid-dsp object to our opaque type.
  struct msresamp_crcf_s *q =
      msresamp_crcf_create(resample_ratio, config->dsp.filter.args.attenuation);

  if (!q) {
    log_fatal("Error: Failed to create liquid-dsp resampler object.");
    return NULL;
  }
  Resampler *resampler =
      (Resampler *)mem_arena_alloc(arena, sizeof(Resampler), true);
  if (!resampler) {
    msresamp_crcf_destroy(q);
    log_fatal("Failed to allocate Resampler state.");
    return NULL;
  }
  resampler->q = q;
  resampler->target_rate = (double)app->dsp.process_chain_sample_rate_hz;
  return resampler;
}

static void resampler_destroy(Resampler *resampler) {
  if (resampler) {
    // Only destroy the liquid-dsp object — the Resampler struct itself is
    // arena-allocated and freed with the arena.
    msresamp_crcf_destroy((msresamp_crcf)resampler->q);
  }
}

static void resampler_reset(Resampler *resampler) {
  if (resampler) {
    msresamp_crcf_reset((msresamp_crcf)resampler->q);
  }
}

static void resampler_execute(Resampler *resampler, ComplexFloat *input,
                              unsigned int num_input_frames,
                              ComplexFloat *output,
                              unsigned int *num_output_frames) {
  if (resampler) {
    msresamp_crcf_execute((msresamp_crcf)resampler->q,
                          (liquid_float_complex *)input, num_input_frames,
                          (liquid_float_complex *)output, num_output_frames);
  }
}

// === DSP Module Interface Implementation ===

static void *dsp_resampler_init(ModuleContext *ctx, double input_rate,
                                double target_output_rate, double *out_rate) {
  float resample_ratio = (float)(target_output_rate / input_rate);
  *out_rate = target_output_rate;

  log_info("Resampling: %.15g Hz -> %.15g Hz (Ratio: %.15g)", input_rate,
           target_output_rate, resample_ratio);

  Resampler *resampler = resampler_create(ctx->config, ctx->app,
                                          &ctx->app->process_chain.setup_arena,
                                          resample_ratio);
  if (resampler)
    resampler->target_rate = target_output_rate;
  return resampler;
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
  chunk->sample_rate = resampler->target_rate;

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

static size_t dsp_resampler_get_ideal_input_size(struct AppContext *app,
                                                 size_t target_output_size) {
  float r = (float)(app->dsp.process_chain_sample_rate_hz /
                    (double)app->module.source_info.sample_rate);
  if (r > 1.0f) {
    return (size_t)(target_output_size / r);
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
