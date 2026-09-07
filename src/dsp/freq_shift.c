#include "app_context.h"
#include "module_defaults.h"
#include "module_registry.h"
#include <stdlib.h>
/**
 * @file frequency_shift.c
 */

#include "app_context.h"
#include "log.h"
#include "module.h"
#include "process_chain_types.h"
#include <liquid.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct freq_shifter_s FreqShifter;

typedef struct FreqShiftState {
  FreqShifter *pre_resample_nco;
  FreqShifter *post_resample_nco;
  double nco_shift_hz;
} FreqShiftState;

static void frequency_shift_destroy_ncos(FreqShiftState *state);
static void frequency_shift_reset_nco(FreqShifter *nco);
static void frequency_shift_apply(FreqShifter *nco, double shift_hz,
                                  ComplexFloat *input, ComplexFloat *output,
                                  unsigned int num_samples);

/**
 * @brief Creates and configures the NCOs (frequency shifters) based on user
 * arguments.
 */
static void *frequency_shift_create(AppConfig *config, AppContext *app) {
  if (!config || !app)
    return NULL;

  FreqShiftState *state = (FreqShiftState *)mem_arena_alloc(
      &app->process_chain.setup_arena, sizeof(FreqShiftState), true);
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
    // free(state); // Memory arena handles this
    return NULL;
  }

  // If no shift is needed, we're done.
  if (fabs(state->nco_shift_hz) < 1e-9) {
    // free(state); // Memory arena handles this
    return NULL;
  }

  // --- Create Pre-Resample NCO ---
  if (!config->dsp.shift_after_resample) {
    double rate_for_nco = (double)app->module.source_info.sample_rate;
    double nyquist_limit = rate_for_nco / 2.0;
    if (fabs(state->nco_shift_hz) > nyquist_limit) {
      log_error("Requested frequency shift %.1f Hz exceeds the Nyquist limit "
                "of %.1f Hz for the input sample rate of %.1f Hz.",
                state->nco_shift_hz, nyquist_limit, rate_for_nco);
      log_error("This will cause aliasing and images.");
      // free(state); // Memory arena handles this
      return NULL;
    }
    state->pre_resample_nco =
        (struct freq_shifter_s *)nco_crcf_create(LIQUID_NCO);
    if (!state->pre_resample_nco) {
      log_error("Failed to create pre-resample NCO (frequency shifter).");
      // free(state); // Memory arena handles this
      return NULL;
    }
    float nco_freq_rad_per_sample =
        (float)(2.0 * M_PI * fabs(state->nco_shift_hz) / rate_for_nco);
    nco_crcf_set_frequency((nco_crcf)state->pre_resample_nco,
                           nco_freq_rad_per_sample);
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
      frequency_shift_destroy_ncos(
          state); // Clean up pre-resample NCO if it was created
      return NULL;
    }
    state->post_resample_nco =
        (struct freq_shifter_s *)nco_crcf_create(LIQUID_NCO);
    if (!state->post_resample_nco) {
      log_error("Failed to create post-resample NCO (frequency shifter).");
      frequency_shift_destroy_ncos(
          state); // Clean up pre-resample NCO if it was created
      return NULL;
    }
    float nco_freq_rad_per_sample =
        (float)(2.0 * M_PI * fabs(state->nco_shift_hz) / rate_for_nco);
    nco_crcf_set_frequency((nco_crcf)state->post_resample_nco,
                           nco_freq_rad_per_sample);
  }

  return state;
}

/**
 * @brief Applies the frequency shift to a block of complex samples using a
 * specific NCO.
 */
static void frequency_shift_apply(FreqShifter *nco, double shift_hz,
                                  ComplexFloat *input_buffer,
                                  ComplexFloat *output_buffer,
                                  unsigned int num_frames) {
  if (!nco || num_frames == 0) {
    return;
  }

  if (shift_hz >= 0) {
    nco_crcf_mix_block_up((nco_crcf)nco, (liquid_float_complex *)input_buffer,
                          (liquid_float_complex *)output_buffer, num_frames);
  } else {
    nco_crcf_mix_block_down((nco_crcf)nco, (liquid_float_complex *)input_buffer,
                            (liquid_float_complex *)output_buffer, num_frames);
  }
}

/**
 * @brief Resets the NCO's phase accumulator without destroying its frequency.
 * This is the safe way to handle stream discontinuities from SDRs.
 */
static void frequency_shift_reset_nco(FreqShifter *nco) {
  if (nco) {
    // This only resets the phase, leaving the frequency configuration intact.
    nco_crcf_set_phase((nco_crcf)nco, 0.0f);
  }
}

/**
 * @brief Destroys the NCO objects if they were created.
 */
static void frequency_shift_destroy_ncos(FreqShiftState *state) {
  if (state) {
    if (state->pre_resample_nco) {
      nco_crcf_destroy((nco_crcf)state->pre_resample_nco);
      state->pre_resample_nco = NULL;
    }
    if (state->post_resample_nco) {
      nco_crcf_destroy((nco_crcf)state->post_resample_nco);
      state->post_resample_nco = NULL;
    }
    // free(state); // Memory arena handles this
  }
}

// === DSP Module Interface Implementation ===

static void *dsp_freq_shift_init(ModuleContext *ctx) {
  return frequency_shift_create((AppConfig *)ctx->config, ctx->app);
}

static SampleChunk *dsp_freq_shift_process(void *state, SampleChunk *chunk) {
  FreqShiftState *fs = (FreqShiftState *)state;
  if (!fs)
    return chunk;

  if (chunk->stream_discontinuity_event) {
    if (fs->pre_resample_nco)
      frequency_shift_reset_nco((FreqShifter *)fs->pre_resample_nco);
    if (fs->post_resample_nco)
      frequency_shift_reset_nco((FreqShifter *)fs->post_resample_nco);
  }

  if (fs->pre_resample_nco) {
    frequency_shift_apply((FreqShifter *)fs->pre_resample_nco, fs->nco_shift_hz,
                          chunk->pre_resample_buffer,
                          chunk->pre_resample_buffer, chunk->frames_read);
  }
  if (fs->post_resample_nco) {
    frequency_shift_apply((FreqShifter *)fs->post_resample_nco,
                          fs->nco_shift_hz, chunk->post_resample_buffer,
                          chunk->post_resample_buffer, chunk->frames_to_write);
  }
  return chunk;
}

static void dsp_freq_shift_cleanup(void *state) {
  frequency_shift_destroy_ncos((FreqShiftState *)state);
}

static void dsp_freq_shift_reset_api(void *state) {
  FreqShiftState *fs = (FreqShiftState *)state;
  if (!fs)
    return;
  if (fs->pre_resample_nco)
    frequency_shift_reset_nco((FreqShifter *)fs->pre_resample_nco);
  if (fs->post_resample_nco)
    frequency_shift_reset_nco((FreqShifter *)fs->post_resample_nco);
}

static double s_frequency_shift_hz = 0.0;
static bool s_shift_after_resample = false;

static const struct argparse_option cli_options[] = {
    OPT_GROUP("Frequency Shift Options"),
    OPT_DOUBLE(0, "freq-shift", &s_frequency_shift_hz,
               "Apply a direct frequency shift in Hz (e.g., -100e3)", NULL, 0,
               0),
    OPT_BOOLEAN(0, "shift-after-resample", &s_shift_after_resample,
                "Apply frequency shift AFTER resampling (default is before)",
                NULL, 0, 0),
};

static const struct argparse_option *
dsp_freq_shift_get_cli_options(int *count) {
  *count = sizeof(cli_options) / sizeof(cli_options[0]);
  return cli_options;
}

static bool dsp_freq_shift_validate_options(struct AppContext *app) {
  if (app && app->config) {
    ((AppConfig *)app->config)->dsp.frequency_shift_hz = s_frequency_shift_hz;
    ((AppConfig *)app->config)->dsp.shift_after_resample =
        s_shift_after_resample;
  }
  return true;
}

static bool dsp_freq_shift_is_active(AppContext *app, const char *stage_tag) {
  AppConfig *config = (AppConfig *)app->config;
  if (config->dsp.frequency_shift_hz == 0.0f) {
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

static const DspModuleInterface dsp_freq_shift_api = {
    .name = "freq_shift",
    .is_active = dsp_freq_shift_is_active,
    .initialize = dsp_freq_shift_init,
    .process = dsp_freq_shift_process,
    .reset = dsp_freq_shift_reset_api,
    .cleanup = dsp_freq_shift_cleanup,
    .validate_options = dsp_freq_shift_validate_options,
    .get_cli_options = dsp_freq_shift_get_cli_options,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "freq_shift",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_freq_shift_api,
  };
  module_registry_add(&m);
}
