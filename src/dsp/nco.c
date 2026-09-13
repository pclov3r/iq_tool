/**
 * @file nco.c
 * @brief Implements Numerically Controlled Oscillator (NCO) frequency shifting for I/Q signals.
 */

#include "app_context.h"
#include "log.h"
#include "module.h"
#include "module_registry.h"
#include "process_chain_types.h"
#include <liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct NcoState {
  nco_crcf pre_resample_nco;
  nco_crcf post_resample_nco;
  double nco_shift_hz;
} NcoState;

static void nco_destroy_ncos(NcoState *state);
static void nco_reset(nco_crcf nco);
static void nco_apply(nco_crcf nco, double shift_hz,
                      ComplexFloat *input, ComplexFloat *output,
                      unsigned int num_samples);

/**
 * @brief Creates and configures the NCOs based on user arguments.
 */
static void *nco_create(AppConfig *config, AppContext *app) {
  if (!config || !app)
    return NULL;

  NcoState *state = (NcoState *)mem_arena_alloc(
      &app->process_chain.setup_arena, sizeof(NcoState), true);
  if (!state)
    return NULL;

  state->pre_resample_nco = NULL;
  state->post_resample_nco = NULL;

  state->nco_shift_hz = app->dsp.nco_shift_hz;
  if (state->nco_shift_hz == 0.0 && config->dsp.frequency_shift_hz != 0.0f) {
    state->nco_shift_hz = config->dsp.frequency_shift_hz;
  }

  // Now that the final shift value is resolved, validate dependent options.
  if (config->dsp.shift_after_resample && fabs(state->nco_shift_hz) < 1e-9) {
    log_error("Option --shift-after-resample was used, but no effective "
              "frequency shift was requested or calculated.");
    return NULL;
  }

  // If no shift is needed, we're done.
  if (fabs(state->nco_shift_hz) < 1e-9) {
    return NULL;
  }

  // --- Create Pre-Resample NCO ---
  if (!config->dsp.shift_after_resample) {
    double rate_for_nco = (double)app->module.input_info.sample_rate;
    double nyquist_limit = rate_for_nco / 2.0;
    if (fabs(state->nco_shift_hz) > nyquist_limit) {
      log_error("Requested frequency shift %.1f Hz exceeds the Nyquist limit "
                "of %.1f Hz for the input sample rate of %.1f Hz.",
                state->nco_shift_hz, nyquist_limit, rate_for_nco);
      log_error("This will cause aliasing and images.");
      return NULL;
    }
    state->pre_resample_nco = nco_crcf_create(LIQUID_NCO);
    if (!state->pre_resample_nco) {
      log_error("Failed to create pre-resample NCO.");
      return NULL;
    }
    float nco_freq_rad_per_sample =
        (float)(2.0 * M_PI * fabs(state->nco_shift_hz) / rate_for_nco);
    nco_crcf_set_frequency(state->pre_resample_nco, nco_freq_rad_per_sample);
  }

  // --- Create Post-Resample NCO ---
  if (config->dsp.shift_after_resample) {
    double rate_for_nco = app->dsp.process_chain_sample_rate_hz;
    double nyquist_limit = rate_for_nco / 2.0;
    if (fabs(state->nco_shift_hz) > nyquist_limit) {
      log_error("Requested frequency shift %.1f Hz exceeds the Nyquist limit "
                "of %.1f Hz for the post-resample rate of %.1f Hz.",
                state->nco_shift_hz, nyquist_limit, rate_for_nco);
      log_error("This will cause aliasing and images.");
      nco_destroy_ncos(state);
      return NULL;
    }
    state->post_resample_nco = nco_crcf_create(LIQUID_NCO);
    if (!state->post_resample_nco) {
      log_error("Failed to create post-resample NCO.");
      nco_destroy_ncos(state);
      return NULL;
    }
    float nco_freq_rad_per_sample =
        (float)(2.0 * M_PI * fabs(state->nco_shift_hz) / rate_for_nco);
    nco_crcf_set_frequency(state->post_resample_nco, nco_freq_rad_per_sample);
  }

  return state;
}

/**
 * @brief Applies the frequency shift to a block of complex samples using a
 * specific NCO.
 */
static void nco_apply(nco_crcf nco, double shift_hz,
                      ComplexFloat *input_buffer,
                      ComplexFloat *output_buffer,
                      unsigned int num_frames) {
  if (!nco || num_frames == 0) {
    return;
  }

  if (shift_hz >= 0) {
    nco_crcf_mix_block_up(nco, (liquid_float_complex *)input_buffer,
                          (liquid_float_complex *)output_buffer, num_frames);
  } else {
    nco_crcf_mix_block_down(nco, (liquid_float_complex *)input_buffer,
                            (liquid_float_complex *)output_buffer, num_frames);
  }
}

/**
 * @brief Resets the NCO's phase accumulator without destroying its frequency.
 * This is the safe way to handle stream discontinuities from SDRs.
 */
static void nco_reset(nco_crcf nco) {
  if (nco) {
    nco_crcf_set_phase(nco, 0.0f);
  }
}

/**
 * @brief Destroys the NCO objects if they were created.
 */
static void nco_destroy_ncos(NcoState *state) {
  if (state) {
    if (state->pre_resample_nco) {
      nco_crcf_destroy(state->pre_resample_nco);
      state->pre_resample_nco = NULL;
    }
    if (state->post_resample_nco) {
      nco_crcf_destroy(state->post_resample_nco);
      state->post_resample_nco = NULL;
    }
  }
}

// === DSP Module Interface Implementation ===

static void *dsp_nco_init(ModuleContext *ctx, double input_rate,
                          double target_output_rate, double *out_rate) {
  (void)target_output_rate;
  *out_rate = input_rate;
  AppConfig *config = (AppConfig *)ctx->config;
  log_info("Enabled (Shift: %.0f Hz, NCO: %.0f Hz)",
           config->dsp.frequency_shift_hz, ctx->app->dsp.nco_shift_hz);
  return nco_create(config, ctx->app);
}

static SampleChunk *dsp_nco_process(void *state, SampleChunk *chunk) {
  NcoState *nco_state = (NcoState *)state;
  if (!nco_state)
    return chunk;

  if (chunk->stream_discontinuity_event) {
    if (nco_state->pre_resample_nco)
      nco_reset(nco_state->pre_resample_nco);
    if (nco_state->post_resample_nco)
      nco_reset(nco_state->post_resample_nco);
  }

  if (nco_state->pre_resample_nco) {
    nco_apply(nco_state->pre_resample_nco, nco_state->nco_shift_hz,
              chunk->current_buffer, chunk->current_buffer,
              chunk->frames_read);
  }
  if (nco_state->post_resample_nco) {
    nco_apply(nco_state->post_resample_nco, nco_state->nco_shift_hz,
              chunk->current_buffer, chunk->current_buffer,
              chunk->frames_to_write);
  }
  return chunk;
}

static void dsp_nco_cleanup(void *state) {
  nco_destroy_ncos((NcoState *)state);
}

static void dsp_nco_reset_api(void *state) {
  NcoState *nco_state = (NcoState *)state;
  if (!nco_state)
    return;
  if (nco_state->pre_resample_nco)
    nco_reset(nco_state->pre_resample_nco);
  if (nco_state->post_resample_nco)
    nco_reset(nco_state->post_resample_nco);
}

static double s_frequency_shift_hz = 0.0;
static bool s_shift_after_resample = false;

// clang-format off
static const struct argparse_option cli_options[] = {
        OPT_GROUP("Frequency Shift Options"),
        OPT_DOUBLE(0, "freq-shift", &s_frequency_shift_hz, "Apply a direct frequency shift in Hz (e.g., -100e3)", NULL, 0, 0),
        OPT_BOOLEAN(0, "shift-after-resample", &s_shift_after_resample, "Apply frequency shift AFTER resampling (default is before)", NULL, 0, 0),
};
// clang-format on

static const struct argparse_option *dsp_nco_get_cli_options(int *count) {
  *count = sizeof(cli_options) / sizeof(cli_options[0]);
  return cli_options;
}

static bool dsp_nco_validate_options(struct AppContext *app) {
  if (app && app->config) {
    ((AppConfig *)app->config)->dsp.frequency_shift_hz = s_frequency_shift_hz;
    ((AppConfig *)app->config)->dsp.shift_after_resample =
        s_shift_after_resample;
  }
  return true;
}

static bool dsp_nco_is_active(AppContext *app, const char *stage_tag) {
  AppConfig *config = (AppConfig *)app->config;
  if (config->dsp.frequency_shift_hz == 0.0f && app->dsp.nco_shift_hz == 0.0) {
    return false;
  }
  if (stage_tag && strcmp(stage_tag, "pre") == 0) {
    return !config->dsp.shift_after_resample;
  }
  if (stage_tag && strcmp(stage_tag, "post") == 0) {
    return config->dsp.shift_after_resample;
  }
  return true; // Default fallback if no tag
}

static const DspModuleInterface dsp_nco_api = {
    .name = "nco",
    .is_active = dsp_nco_is_active,
    .initialize = dsp_nco_init,
    .process = dsp_nco_process,
    .reset = dsp_nco_reset_api,
    .cleanup = dsp_nco_cleanup,
    .validate_options = dsp_nco_validate_options,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "nco",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_nco_api,
      .get_cli_options = dsp_nco_get_cli_options,
  };
  module_registry_add(&m);
}
