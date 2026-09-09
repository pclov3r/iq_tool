/**
 * @file filter.c
 * @brief Implements the user-defined FIR/FFT filter chain.
 */

#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "mem_arena.h"
#include "module.h"
#include "module_registry.h"
#include "process_chain_types.h"
#include "sample_format_table.h"
#include "utilities.h"
#include <ctype.h>
#include <liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Filter Design & Analysis Tuning ---
#define FILTER_MINIMUM_TAPS 21
#define FILTER_MAXIMUM_AUTO_TAPS 65536
#define FILTER_SAFETY_DEFAULT_TAPS FILTER_MAXIMUM_AUTO_TAPS
#define FILTER_GAIN_ZERO_THRESHOLD 1e-9f
#define FILTER_FREQ_RESPONSE_POINTS 2048

typedef struct FilterState {
  struct liquid_filter_s *object;
  int type_actual;
  unsigned int block_size;
  ComplexFloat *pre_fft_remainder_buffer;
  unsigned int pre_fft_remainder_length;
  ComplexFloat *post_fft_remainder_buffer;
  unsigned int post_fft_remainder_length;
  ComplexFloat *fft_scratch_buffer;
  bool apply_post_resample;
} FilterState;

// --- Create a real-coefficient (_crcf) filter from the complex master taps ---
#define PREPARE_AND_CREATE_CRCF_FILTER(prefix, ...)                            \
  do {                                                                         \
    float *final_real_taps = (float *)mem_arena_alloc(                         \
        arena, master_taps_length * sizeof(float), false);                     \
    if (!final_real_taps)                                                      \
      goto cleanup;                                                            \
    for (int i = 0; i < master_taps_length; i++) {                             \
      final_real_taps[i] = crealf(master_taps[i]);                             \
    }                                                                          \
    state->object = (struct liquid_filter_s *)prefix##_crcf_create(            \
        final_real_taps, master_taps_length, ##__VA_ARGS__);                   \
  } while (0)

// --- Static Helper Functions ---

/**
 * @brief Determines if the filter should be applied before or after resampling
 * for efficiency. This is an important optimization. If downsampling, it checks
 * if the filter's passband is within the Nyquist frequency of the *output*
 * rate. If so, it's more efficient to filter after resampling. This function
 * modifies the config.
 * @param config The application configuration.
 * @param app The application app.
 * @return true on success, false if the filter configuration is invalid for the
 * output rate.
 */
static bool _configure_filter_stage(AppConfig *config, AppContext *app) {
  config->dsp.filter.apply_post_resample = false;

  if (config->dsp.filter.count == 0 || config->dsp.raw_passthrough) {
    return true;
  }

  double input_rate = (double)app->module.source_info.sample_rate;
  double output_sample_rate = app->dsp.process_chain_sample_rate_hz;

  // This optimization is only relevant if we are downsampling.
  if (output_sample_rate < input_rate) {
    float max_filter_freq_hz = 0.0f;

    // Find the highest frequency required by any filter in the chain.
    for (int i = 0; i < config->dsp.filter.count; i++) {
      const FilterRequest *request = &config->dsp.filter.requests[i];
      float current_max = 0.0f;
      switch (request->type) {
      case FILTER_TYPE_LOWPASS:
      case FILTER_TYPE_HIGHPASS:
        current_max = fabsf(request->freq1_hz);
        break;
      case FILTER_TYPE_PASSBAND:
      case FILTER_TYPE_STOPBAND:
        current_max = fabsf(request->freq1_hz) + (request->freq2_hz / 2.0f);
        break;
      default:
        break;
      }
      if (current_max > max_filter_freq_hz) {
        max_filter_freq_hz = current_max;
      }
    }

    double output_nyquist = output_sample_rate / 2.0;

    if (max_filter_freq_hz > output_nyquist) {
      log_error(
          "Filter configuration is incompatible with the output sample rate.");
      log_error(
          "The specified filter chain extends to %.15g Hz, but the output rate "
          "of %.15g Hz can only support frequencies up to %.15g Hz.",
          max_filter_freq_hz, output_sample_rate, output_nyquist);
      return false;
    } else {
      // It's safe and more efficient to filter after resampling.
      log_debug("Filter will be applied efficiently after resampling to avoid "
                "excessive CPU usage.");
      config->dsp.filter.apply_post_resample = true;
    }
  }
  return true;
}

static inline void _normalize_filter_dc_gain(float *taps, unsigned int length) {
  float sum = 0.0f;
  for (unsigned int k = 0; k < length; k++)
    sum += taps[k];
  if (sum != 0.0f) {
    for (unsigned int k = 0; k < length; k++)
      taps[k] /= sum;
  }
}

static inline void _invert_filter_spectrum(float *taps, unsigned int length) {
  for (unsigned int k = 0; k < length; k++) {
    taps[k] = -taps[k];
  }
  taps[(length - 1) / 2] += 1.0f;
}

static unsigned int _execute_fft_filter_pass(
    struct liquid_filter_s *filter_object, FilterImplementationType filter_type,
    const ComplexFloat *input_buffer, unsigned int frames_in,
    ComplexFloat *output_buffer, ComplexFloat *remainder_buffer,
    unsigned int *remainder_length_ptr, unsigned int block_size,
    ComplexFloat *scratch_buffer, bool is_last_chunk);

static liquid_float_complex *
convolve_complex_taps(const liquid_float_complex *h1, int length1,
                      const liquid_float_complex *h2, int length2,
                      int *out_length, MemoryArena *arena) {
  *out_length = length1 + length2 - 1;
  liquid_float_complex *result = (liquid_float_complex *)mem_arena_alloc(
      arena, *out_length * sizeof(liquid_float_complex), false);
  if (!result) {
    return NULL;
  }

  memset(result, 0, *out_length * sizeof(liquid_float_complex));

  for (int i = 0; i < *out_length; i++) {
    int j_start = (i >= length1) ? (i - length1 + 1) : 0;
    int j_end = (i < length2 - 1) ? i : (length2 - 1);

    for (int j = j_start; j <= j_end; j++) {
      result[i] += h1[i - j] * h2[j];
    }
  }
  return result;
}

static liquid_float_complex *
_generate_base_lowpass_taps(unsigned int taps_length, float half_bw_norm,
                            float attenuation_db, MemoryArena *arena) {
  float *real_taps =
      (float *)mem_arena_alloc(arena, taps_length * sizeof(float), false);
  if (!real_taps)
    return NULL;
  memset(real_taps, 0, taps_length * sizeof(float));
  liquid_firdes_kaiser(taps_length, half_bw_norm, attenuation_db, 0.0f,
                       real_taps);
  _normalize_filter_dc_gain(real_taps, taps_length);

  liquid_float_complex *complex_taps = (liquid_float_complex *)mem_arena_alloc(
      arena, taps_length * sizeof(liquid_float_complex), false);
  if (!complex_taps)
    return NULL;
  memset(complex_taps, 0, taps_length * sizeof(liquid_float_complex));
  for (unsigned int k = 0; k < taps_length; k++) {
    complex_taps[k] = real_taps[k] + 0.0f * I;
  }
  return complex_taps;
}

static void _apply_complex_nco_shift(liquid_float_complex *taps,
                                     unsigned int taps_length, float fc_norm) {
  nco_crcf shifter = nco_crcf_create(LIQUID_NCO);
  nco_crcf_set_frequency(shifter, 2.0f * M_PI * fc_norm);

  // Critical DSP Fix: Set initial phase so the center tap has 0 phase offset.
  float m_idx = (float)(taps_length - 1) / 2.0f;
  nco_crcf_set_phase(shifter, -(2.0f * M_PI * fc_norm) * m_idx);

  for (unsigned int k = 0; k < taps_length; k++) {
    liquid_float_complex shift_val;
    nco_crcf_cexpf(shifter, &shift_val);
    taps[k] *= shift_val;
    nco_crcf_step(shifter);
  }
  nco_crcf_destroy(shifter);
}

static void _invert_to_highpass_or_notch(liquid_float_complex *taps,
                                         unsigned int taps_length) {
  for (unsigned int k = 0; k < taps_length; k++) {
    taps[k] = -taps[k];
  }
  taps[(taps_length - 1) / 2] += 1.0f + 0.0f * I;
}

static liquid_float_complex *
_compound_filter_stages(AppConfig *config, double sample_rate,
                        int *master_length, bool *out_is_complex,
                        bool *out_norm_peak, MemoryArena *arena) {
  int m_length = 1;
  liquid_float_complex *master_taps = (liquid_float_complex *)mem_arena_alloc(
      arena, sizeof(liquid_float_complex), false);
  if (!master_taps)
    return NULL;
  master_taps[0] = 1.0f + 0.0f * I;

  *out_is_complex = false;
  *out_norm_peak = false;

  for (int i = 0; i < config->dsp.filter.count; ++i) {
    FilterRequest *request = &config->dsp.filter.requests[i];

    if (request->type != FILTER_TYPE_LOWPASS) {
      *out_norm_peak = true;
    }

    unsigned int current_taps_length;
    float atten = config->dsp.filter.args.attenuation;

    if (config->dsp.filter.args.taps > 0) {
      current_taps_length = (unsigned int)config->dsp.filter.args.taps;
      if (current_taps_length % 2 == 0)
        current_taps_length++;
    } else {
      float tw_hz = config->dsp.filter.args.transition_width;
      if (tw_hz <= 0.0f) {
        float ref_freq = (request->type == FILTER_TYPE_LOWPASS ||
                          request->type == FILTER_TYPE_HIGHPASS)
                             ? request->freq1_hz
                             : request->freq2_hz;
        tw_hz = fabsf(ref_freq) * DEFAULT_FILTER_TRANSITION_FACTOR;
      }
      if (tw_hz < 1.0f)
        tw_hz = 1.0f;
      current_taps_length =
          estimate_req_filter_len(tw_hz / (float)sample_rate, atten);
      if (current_taps_length % 2 == 0)
        current_taps_length++;
      if (current_taps_length < FILTER_MINIMUM_TAPS)
        current_taps_length = FILTER_MINIMUM_TAPS;

      if (current_taps_length > FILTER_MAXIMUM_AUTO_TAPS) {
        current_taps_length = FILTER_MAXIMUM_AUTO_TAPS;
      }
    }

    bool is_complex = ((request->type == FILTER_TYPE_PASSBAND ||
                        request->type == FILTER_TYPE_STOPBAND) &&
                       fabsf(request->freq1_hz) > 1e-9f);
    if (is_complex)
      *out_is_complex = true;

    float bw_norm = 0.0f;
    if (request->type == FILTER_TYPE_LOWPASS ||
        request->type == FILTER_TYPE_HIGHPASS) {
      bw_norm = request->freq1_hz / (float)sample_rate;
    } else {
      bw_norm = (request->freq2_hz / 2.0f) / (float)sample_rate;
    }

    liquid_float_complex *current_taps =
        _generate_base_lowpass_taps(current_taps_length, bw_norm, atten, arena);
    if (!current_taps)
      return NULL;

    if (is_complex) {
      _apply_complex_nco_shift(current_taps, current_taps_length,
                               request->freq1_hz / (float)sample_rate);
    }

    if (request->type == FILTER_TYPE_HIGHPASS ||
        request->type == FILTER_TYPE_STOPBAND) {
      _invert_to_highpass_or_notch(current_taps, current_taps_length);
    }

    int new_length;
    liquid_float_complex *new_master =
        convolve_complex_taps(master_taps, m_length, current_taps,
                              current_taps_length, &new_length, arena);
    if (!new_master)
      return NULL;

    master_taps = new_master;
    m_length = new_length;
  }

  *master_length = m_length;
  return master_taps;
}

static struct liquid_filter_s *_compile_filter_object(
    AppConfig *config, FilterState *state, liquid_float_complex *master_taps,
    int master_taps_length, bool is_final_filter_complex,
    bool normalize_by_peak, MemoryArena *arena, size_t alloc_size_samples) {
  if (normalize_by_peak || is_final_filter_complex) {
    log_info("Normalizing filter gain (this may be slow for large filters)...");
    float max_mag = 0.0f;
    firfilt_cccf temp_filter =
        firfilt_cccf_create(master_taps, master_taps_length);
    if (temp_filter) {
      liquid_float_complex H;
      for (int i = 0; i < FILTER_FREQ_RESPONSE_POINTS; i++) {
        float freq = ((float)i / (float)FILTER_FREQ_RESPONSE_POINTS) - 0.5f;
        firfilt_cccf_freqresponse(temp_filter, freq, &H);
        float mag = cabsf(H);
        if (mag > max_mag)
          max_mag = mag;
      }
      firfilt_cccf_destroy(temp_filter);
    }
    if (max_mag > FILTER_GAIN_ZERO_THRESHOLD) {
      for (int i = 0; i < master_taps_length; i++)
        master_taps[i] /= max_mag;
    }
  } else {
    double gain_correction = 0.0;
    for (int i = 0; i < master_taps_length; i++)
      gain_correction += crealf(master_taps[i]);
    if (fabs(gain_correction) > FILTER_GAIN_ZERO_THRESHOLD) {
      for (int i = 0; i < master_taps_length; i++)
        master_taps[i] /= (float)gain_correction;
    }
  }

  FilterTypeRequest final_choice;
  if (config->dsp.filter.args.type_str != NULL) {
    final_choice = config->dsp.filter.type_req;
  } else {
    final_choice = is_final_filter_complex ? FILTER_TYPE_FFT : FILTER_TYPE_FIR;
    log_info(is_final_filter_complex
                 ? "Automatically choosing efficient FFT method by default."
                 : "Symmetric filter detected. Using default low-latency FIR "
                   "method.");
  }

  if (final_choice == FILTER_TYPE_FFT) {
    log_info("Preparing FFT-based filter object (this may take a moment)...");
    unsigned int block_size;
    if (config->dsp.filter.args.fft_size > 0) {
      block_size = (unsigned int)config->dsp.filter.args.fft_size / 2;
      if (block_size < (unsigned int)master_taps_length - 1)
        return NULL;
    } else {
      block_size = 1;
      while (block_size < (unsigned int)master_taps_length - 1)
        block_size *= 2;
      if (block_size < (unsigned int)master_taps_length * 2)
        block_size *= 2;
    }
    state->block_size = block_size;

    if (is_final_filter_complex) {
      state->type_actual = FILTER_IMPL_FFT_ASYMMETRIC;
      state->object = (struct liquid_filter_s *)fftfilt_cccf_create(
          master_taps, master_taps_length, block_size);
    } else {
      float *real_taps = (float *)mem_arena_alloc(
          arena, master_taps_length * sizeof(float), false);
      for (int i = 0; i < master_taps_length; i++)
        real_taps[i] = crealf(master_taps[i]);
      state->type_actual = FILTER_IMPL_FFT_SYMMETRIC;
      state->object = (struct liquid_filter_s *)fftfilt_crcf_create(
          real_taps, master_taps_length, block_size);
    }

    size_t scratch_needed = alloc_size_samples + state->block_size + 64;
    state->fft_scratch_buffer = (ComplexFloat *)mem_arena_alloc(
        arena, scratch_needed * sizeof(ComplexFloat), true);
    if (!state->fft_scratch_buffer)
      return NULL;
  } else {
    log_info("Preparing FIR (time-domain) filter object...");
    if (is_final_filter_complex) {
      state->type_actual = FILTER_IMPL_FIR_ASYMMETRIC;
      state->object = (struct liquid_filter_s *)firfilt_cccf_create(
          master_taps, master_taps_length);
    } else {
      float *real_taps = (float *)mem_arena_alloc(
          arena, master_taps_length * sizeof(float), false);
      for (int i = 0; i < master_taps_length; i++)
        real_taps[i] = crealf(master_taps[i]);
      state->type_actual = FILTER_IMPL_FIR_SYMMETRIC;
      state->object = (struct liquid_filter_s *)firfilt_crcf_create(
          real_taps, master_taps_length);
    }
  }
  return state->object;
}

static void *filter_create(AppConfig *config, AppContext *app,
                           MemoryArena *arena) {
  if (config->dsp.filter.count == 0)
    return NULL;

  FilterState *state =
      (FilterState *)mem_arena_alloc(arena, sizeof(FilterState), true);
  if (!state)
    return NULL;

  state->object = NULL;
  state->type_actual = FILTER_IMPL_NONE;
  state->block_size = 0;

  if (!_configure_filter_stage(config, app))
    return NULL;

  double sample_rate = config->dsp.filter.apply_post_resample
                           ? app->dsp.process_chain_sample_rate_hz
                           : (double)app->module.source_info.sample_rate;

  int master_length = 0;
  bool is_complex = false, norm_peak = false;
  log_info(
      "Designing filter coefficients (this may be slow for large filters)...");

  liquid_float_complex *master_taps = _compound_filter_stages(
      config, sample_rate, &master_length, &is_complex, &norm_peak, arena);
  if (!master_taps)
    return NULL;

  if (config->dsp.filter.args.taps > 0) {
    log_info("Using user-specified filter size of %d taps.", master_length);
  } else if (master_length >= FILTER_MAXIMUM_AUTO_TAPS) {
    log_info("Final combined filter clamped to maximum %d taps.",
             master_length);
  } else {
    log_info("Final combined filter requires %d taps.", master_length);
  }

  if (is_complex)
    log_info("Asymmetric filter detected.");

  if (!_compile_filter_object(config, state, master_taps, master_length,
                              is_complex, norm_peak, arena,
                              app->process_chain.alloc_size_samples)) {
    log_fatal("Failed to create final combined filter object.");
    return NULL;
  }

  if (config->dsp.filter.apply_post_resample) {
    state->post_fft_remainder_buffer = (ComplexFloat *)mem_arena_alloc(
        arena, state->block_size * sizeof(ComplexFloat), true);
    state->post_fft_remainder_length = 0;
  } else {
    state->pre_fft_remainder_buffer = (ComplexFloat *)mem_arena_alloc(
        arena, state->block_size * sizeof(ComplexFloat), true);
    state->pre_fft_remainder_length = 0;
  }

  state->apply_post_resample = config->dsp.filter.apply_post_resample;

  return state;
}

static void filter_destroy(FilterState *state) {
  if (state && state->object) {
    switch (state->type_actual) {
    case FILTER_IMPL_FIR_SYMMETRIC:
      firfilt_crcf_destroy((firfilt_crcf)state->object);
      break;
    case FILTER_IMPL_FIR_ASYMMETRIC:
      firfilt_cccf_destroy((firfilt_cccf)state->object);
      break;
    case FILTER_IMPL_FFT_SYMMETRIC:
      fftfilt_crcf_destroy((fftfilt_crcf)state->object);
      break;
    case FILTER_IMPL_FFT_ASYMMETRIC:
      fftfilt_cccf_destroy((fftfilt_cccf)state->object);
      break;
    default:
      break;
    }
    state->object = NULL;
  }
}

static void filter_reset(FilterState *state) {
  if (state && state->object) {
    switch (state->type_actual) {
    case FILTER_IMPL_FIR_SYMMETRIC:
      firfilt_crcf_reset((firfilt_crcf)state->object);
      break;
    case FILTER_IMPL_FIR_ASYMMETRIC:
      firfilt_cccf_reset((firfilt_cccf)state->object);
      break;
    case FILTER_IMPL_FFT_SYMMETRIC:
      fftfilt_crcf_reset((fftfilt_crcf)state->object);
      break;
    case FILTER_IMPL_FFT_ASYMMETRIC:
      fftfilt_cccf_reset((fftfilt_cccf)state->object);
      break;
    default:
      break;
    }
  }
}

static unsigned int filter_apply(FilterState *state, SampleChunk *item,
                                 bool is_post_resample) {
  if (!state || !state->object) {
    return is_post_resample ? item->frames_to_write : item->frames_read;
  }

  unsigned int frames_in =
      is_post_resample ? item->frames_to_write : item->frames_read;
  ComplexFloat *target_buffer = item->current_buffer;
  if (frames_in == 0) {
    return 0;
  }

  switch (state->type_actual) {
  case FILTER_IMPL_FIR_SYMMETRIC:
  case FILTER_IMPL_FIR_ASYMMETRIC:
    if (state->type_actual == FILTER_IMPL_FIR_SYMMETRIC) {
      firfilt_crcf_execute_block(
          (firfilt_crcf)state->object, (liquid_float_complex *)target_buffer,
          frames_in, (liquid_float_complex *)target_buffer);
    } else {
      firfilt_cccf_execute_block(
          (firfilt_cccf)state->object, (liquid_float_complex *)target_buffer,
          frames_in, (liquid_float_complex *)target_buffer);
    }
    return frames_in;

  case FILTER_IMPL_FFT_SYMMETRIC:
  case FILTER_IMPL_FFT_ASYMMETRIC: {
    ComplexFloat *remainder_buffer = is_post_resample
                                         ? state->post_fft_remainder_buffer
                                         : state->pre_fft_remainder_buffer;
    unsigned int *remainder_length_ptr = is_post_resample
                                             ? &state->post_fft_remainder_length
                                             : &state->pre_fft_remainder_length;

    unsigned int output_frames = _execute_fft_filter_pass(
        state->object, state->type_actual, target_buffer, frames_in,
        target_buffer, remainder_buffer, remainder_length_ptr,
        state->block_size, state->fft_scratch_buffer, item->is_last_chunk);

    return output_frames;
  }

  default:
    return frames_in;
  }
}

// Actual definition of the helper function
static unsigned int _execute_fft_filter_pass(
    struct liquid_filter_s *filter_object, FilterImplementationType filter_type,
    const ComplexFloat *input_buffer, unsigned int frames_in,
    ComplexFloat *output_buffer, ComplexFloat *remainder_buffer,
    unsigned int *remainder_length_ptr, unsigned int block_size,
    ComplexFloat *scratch_buffer, bool is_last_chunk) {
  unsigned int old_remainder_length = *remainder_length_ptr;
  unsigned int total_frames_to_process = old_remainder_length + frames_in;

  memcpy(scratch_buffer, remainder_buffer,
         old_remainder_length * sizeof(ComplexFloat));
  memcpy(scratch_buffer + old_remainder_length, input_buffer,
         frames_in * sizeof(ComplexFloat));

  unsigned int processed_frames = 0;
  unsigned int total_output_frames = 0;
  while (total_frames_to_process >= processed_frames + block_size) {
    if (filter_type == FILTER_IMPL_FFT_SYMMETRIC) {
      fftfilt_crcf_execute(
          (fftfilt_crcf)filter_object,
          (liquid_float_complex *)(scratch_buffer + processed_frames),
          (liquid_float_complex *)(output_buffer + total_output_frames));
    } else {
      fftfilt_cccf_execute(
          (fftfilt_cccf)filter_object,
          (liquid_float_complex *)(scratch_buffer + processed_frames),
          (liquid_float_complex *)(output_buffer + total_output_frames));
    }
    processed_frames += block_size;
    total_output_frames += block_size;
  }

  // EOF LOGIC: Flush the remaining tail of the file
  if (is_last_chunk && (total_frames_to_process > processed_frames)) {
    unsigned int final_tail_length = total_frames_to_process - processed_frames;

    // Zero-pad the remaining samples up to block_size
    memset(scratch_buffer + processed_frames + final_tail_length, 0,
           (block_size - final_tail_length) * sizeof(ComplexFloat));

    // Force one final FFT execution
    if (filter_type == FILTER_IMPL_FFT_SYMMETRIC) {
      fftfilt_crcf_execute(
          (fftfilt_crcf)filter_object,
          (liquid_float_complex *)(scratch_buffer + processed_frames),
          (liquid_float_complex *)(output_buffer + total_output_frames));
    } else {
      fftfilt_cccf_execute(
          (fftfilt_cccf)filter_object,
          (liquid_float_complex *)(scratch_buffer + processed_frames),
          (liquid_float_complex *)(output_buffer + total_output_frames));
    }

    processed_frames +=
        final_tail_length; // Only advance by the actual data length
    total_output_frames +=
        final_tail_length; // Only output the actual data length
  }

  unsigned int new_remainder_length =
      total_frames_to_process - processed_frames;
  if (new_remainder_length > 0) {
    memmove(remainder_buffer, scratch_buffer + processed_frames,
            new_remainder_length * sizeof(ComplexFloat));
  }
  *remainder_length_ptr = new_remainder_length;

  return total_output_frames;
}

static float s_lowpass[FILTER_MAX_CHAIN] = {0};
static float s_highpass[FILTER_MAX_CHAIN] = {0};
static const char *s_pass_range[FILTER_MAX_CHAIN] = {0};
static const char *s_stopband[FILTER_MAX_CHAIN] = {0};
static float s_transition_width = 0.0f;
static int s_filter_taps = 0;
static float s_attenuation = 0.0f;
static const char *s_filter_type_str = NULL;
static int s_filter_fft_size = 0;

// clang-format off
static const struct argparse_option cli_options[] = {
        OPT_GROUP("Filtering Options (Chain up to 5 by combining options or adding " "suffixes -2, -3, etc. e.g., --lowpass --stopband --lowpass-2 " "--pass-range --pass-range-2)"),
        OPT_FLOAT(0, "lowpass", &s_lowpass[0], "Isolate signal at DC. Keeps freqs from -<hz> to +<hz>.", NULL, 0, 0),
        OPT_FLOAT(0, "lowpass-2", &s_lowpass[1], NULL, NULL, 0, 0),
        OPT_FLOAT(0, "lowpass-3", &s_lowpass[2], NULL, NULL, 0, 0),
        OPT_FLOAT(0, "lowpass-4", &s_lowpass[3], NULL, NULL, 0, 0),
        OPT_FLOAT(0, "lowpass-5", &s_lowpass[4], NULL, NULL, 0, 0),
        OPT_FLOAT(0, "highpass", &s_highpass[0], "Remove signal at DC. Rejects freqs from -<hz> to +<hz>.", NULL, 0, 0),
        OPT_FLOAT(0, "highpass-2", &s_highpass[1], NULL, NULL, 0, 0),
        OPT_FLOAT(0, "highpass-3", &s_highpass[2], NULL, NULL, 0, 0),
        OPT_FLOAT(0, "highpass-4", &s_highpass[3], NULL, NULL, 0, 0),
        OPT_FLOAT(0, "highpass-5", &s_highpass[4], NULL, NULL, 0, 0),
        OPT_STRING(0, "pass-range", &s_pass_range[0], "Isolate a specific band. Format: 'start_freq:end_freq'.", NULL, 0, 0),
        OPT_STRING(0, "pass-range-2", &s_pass_range[1], NULL, NULL, 0, 0),
        OPT_STRING(0, "pass-range-3", &s_pass_range[2], NULL, NULL, 0, 0),
        OPT_STRING(0, "pass-range-4", &s_pass_range[3], NULL, NULL, 0, 0),
        OPT_STRING(0, "pass-range-5", &s_pass_range[4], NULL, NULL, 0, 0),
        OPT_STRING(0, "stopband", &s_stopband[0], "Remove a specific band (notch). Format: 'start_freq:end_freq'.", NULL, 0, 0),
        OPT_STRING(0, "stopband-2", &s_stopband[1], NULL, NULL, 0, 0),
        OPT_STRING(0, "stopband-3", &s_stopband[2], NULL, NULL, 0, 0),
        OPT_STRING(0, "stopband-4", &s_stopband[3], NULL, NULL, 0, 0),
        OPT_STRING(0, "stopband-5", &s_stopband[4], NULL, NULL, 0, 0),

        OPT_GROUP("Filter Quality Options"),
        OPT_FLOAT( 0, "transition-width", &s_transition_width, "Set filter sharpness by transition width in Hz. (Default: Auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-taps", &s_filter_taps, "Set exact filter length. Overrides --transition-width.", NULL, 0, 0),
        OPT_FLOAT(0, "attenuation", &s_attenuation, "Set global filter & resampler stop-band attenuation in dB. " "(Default: Auto).", NULL, 0, 0),

        OPT_GROUP("Filter Implementation Options"),
        OPT_STRING(0, "filter-type", &s_filter_type_str, "Set filter implementation {fir|fft}. (Default: auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-fft-size", &s_filter_fft_size, "Set FFT size for 'fft' filter type. Must be a power of 2.", NULL, 0, 0),
};
// clang-format on

static const struct argparse_option *dsp_filter_get_cli_options(int *count) {
  *count = sizeof(cli_options) / sizeof(cli_options[0]);
  return cli_options;
}

static bool parse_start_end_string(const char *input_str, const char *arg_name,
                                   float *out_start, float *out_end) {
  char start_buffer[128], end_buffer[128];
  if (sscanf(input_str, "%127[^:]:%127s", start_buffer, end_buffer) != 2) {
    log_error(
        "Invalid format for %s. Expected 'start_freq:end_freq'. Found '%s'.",
        arg_name, input_str);
    return false;
  }
  char *endptr1;
  char *endptr2;
  *out_start = strtof(start_buffer, &endptr1);
  *out_end = strtof(end_buffer, &endptr2);
  if (*endptr1 != '\0' || *endptr2 != '\0') {
    log_fatal("Invalid numerical value in %s argument. Could not parse '%s'.",
              arg_name, input_str);
    return false;
  }
  if (*out_end <= *out_start) {
    log_error(
        "In %s argument, end frequency must be greater than start frequency.",
        arg_name);
    return false;
  }
  return true;
}

static void add_filter_request(AppConfig *config, FilterType type, float f1,
                               float f2) {
  if (config->dsp.filter.count < FILTER_MAX_CHAIN) {
    config->dsp.filter.requests[config->dsp.filter.count] =
        (FilterRequest){.type = type, .freq1_hz = f1, .freq2_hz = f2};
    config->dsp.filter.count++;
  } else {
    log_warn("Maximum number of chained filters (%d) reached. Ignoring further "
             "filter options.",
             FILTER_MAX_CHAIN);
  }
}

static bool dsp_filter_validate_options(struct AppContext *app) {
  if (app && app->config) {
    AppConfig *config = (AppConfig *)app->config;
    for (int i = 0; i < FILTER_MAX_CHAIN; i++) {
      config->dsp.filter.args.lowpass[i] = s_lowpass[i];
      config->dsp.filter.args.highpass[i] = s_highpass[i];
      config->dsp.filter.args.pass_range[i] = s_pass_range[i];
      config->dsp.filter.args.stopband[i] = s_stopband[i];
    }
    config->dsp.filter.args.transition_width = s_transition_width;
    config->dsp.filter.args.taps = s_filter_taps;
    config->dsp.filter.args.attenuation = s_attenuation;
    config->dsp.filter.args.type_str = s_filter_type_str;
    config->dsp.filter.args.fft_size = s_filter_fft_size;

    config->dsp.filter.count = 0;
    for (int i = 0; i < FILTER_MAX_CHAIN; i++) {
      if (config->dsp.filter.args.lowpass[i] > 0.0f)
        add_filter_request(config, FILTER_TYPE_LOWPASS,
                           config->dsp.filter.args.lowpass[i], 0.0f);
      if (config->dsp.filter.args.highpass[i] > 0.0f)
        add_filter_request(config, FILTER_TYPE_HIGHPASS,
                           config->dsp.filter.args.highpass[i], 0.0f);
      if (config->dsp.filter.args.pass_range[i]) {
        float start_f, end_f;
        if (!parse_start_end_string(config->dsp.filter.args.pass_range[i],
                                    "--pass-range", &start_f, &end_f))
          return false;
        add_filter_request(config, FILTER_TYPE_PASSBAND,
                           start_f + ((end_f - start_f) / 2.0f),
                           end_f - start_f);
      }
      if (config->dsp.filter.args.stopband[i]) {
        float start_f, end_f;
        if (!parse_start_end_string(config->dsp.filter.args.stopband[i],
                                    "--stopband", &start_f, &end_f))
          return false;
        add_filter_request(config, FILTER_TYPE_STOPBAND,
                           start_f + ((end_f - start_f) / 2.0f),
                           end_f - start_f);
      }
    }

    if (config->dsp.filter.args.transition_width > 0.0f &&
        config->dsp.filter.args.taps > 0) {
      log_error("Cannot specify both --transition-width and --filter-taps.");
      return false;
    }
    if (config->dsp.filter.args.taps != 0 && config->dsp.filter.args.taps < 3) {
      log_error("--filter-taps must be 3 or greater.");
      return false;
    }
    if (config->dsp.filter.args.taps != 0 &&
        config->dsp.filter.args.taps % 2 == 0) {
      log_warn("--filter-taps must be odd. Adjusting from %d to %d.",
               config->dsp.filter.args.taps, config->dsp.filter.args.taps + 1);
      config->dsp.filter.args.taps++;
    }
    if (config->dsp.filter.args.attenuation <= 0.0f &&
        config->dsp.filter.args.attenuation != 0.0f) {
      log_error("--attenuation must be a positive value.");
      return false;
    }
    if (config->dsp.filter.args.attenuation == 0.0f) {
      float resolved_attenuation = 60.0f;
      const Module *in_mod =
          module_get(config->input.type_name, MODULE_TYPE_INPUT, NULL);
      if (in_mod) {
        resolved_attenuation =
            (in_mod->default_filter_attenuation_db > 0.0f)
                ? in_mod->default_filter_attenuation_db
                : (get_format_info_by_enum(config->output.sample_format)
                       ? get_format_info_by_enum(config->output.sample_format)
                             ->default_filter_attenuation_db
                       : 60.0f);
      }
      config->dsp.filter.args.attenuation = resolved_attenuation;
    }

    // Also move the validate_option_combinations filter logic here
    if (config->dsp.filter.args.type_str) {
      if (strcasecmp(config->dsp.filter.args.type_str, "fir") == 0)
        config->dsp.filter.type_req = FILTER_TYPE_FIR;
      else if (strcasecmp(config->dsp.filter.args.type_str, "fft") == 0)
        config->dsp.filter.type_req = FILTER_TYPE_FFT;
      else {
        log_error(
            "Invalid value for --filter-type: '%s'. Must be 'fir' or 'fft'.",
            config->dsp.filter.args.type_str);
        return false;
      }
    }
    if (config->dsp.filter.args.fft_size != 0) {
      if (config->dsp.filter.args.type_str &&
          config->dsp.filter.type_req == FILTER_TYPE_FIR) {
        log_error("--filter-fft-size cannot be used with an explicit "
                  "'--filter-type fir'.");
        return false;
      }
      if (config->dsp.filter.type_req != FILTER_TYPE_FFT) {
        log_debug("--filter-fft-size forces filter type to FFT.");
        config->dsp.filter.type_req = FILTER_TYPE_FFT;
      }
      int n = config->dsp.filter.args.fft_size;
      if (n <= 0 || ((n & (n - 1)) != 0)) {
        log_error("--filter-fft-size must be a positive power of two.");
        return false;
      }
    }
  }
  return true;
}

static void filter_get_summary_info(void *state,
                                    OutputSummaryInfo *output_info) {
  FilterState *fs = (FilterState *)state;
  if (!fs) {
    utility_add_summary_item(output_info, "Filter", "Disabled");
  } else {
    const char *filter_label;
    switch (fs->type_actual) {
    case FILTER_IMPL_FIR_SYMMETRIC:
      filter_label = "FIR (Symmetric, Time-Domain)";
      break;
    case FILTER_IMPL_FIR_ASYMMETRIC:
      filter_label = "FIR (Asymmetric, Time-Domain)";
      break;
    case FILTER_IMPL_FFT_SYMMETRIC:
      filter_label = "FFT (Symmetric, Frequency-Domain)";
      break;
    case FILTER_IMPL_FFT_ASYMMETRIC:
      filter_label = "FFT (Asymmetric, Frequency-Domain)";
      break;
    default:
      filter_label = "None";
      break;
    }
    utility_add_summary_item(output_info, "Filter", "%s", filter_label);
  }
}

// === DSP Module Interface Implementation ===

static void *dsp_filter_init(ModuleContext *ctx, double input_rate,
                             double target_output_rate, double *out_rate) {
  (void)target_output_rate;
  *out_rate = input_rate;
  return filter_create((AppConfig *)ctx->config, ctx->app,
                       &ctx->app->process_chain.setup_arena);
}

static SampleChunk *dsp_filter_process(void *state, SampleChunk *chunk) {
  FilterState *fs = (FilterState *)state;
  if (!fs)
    return chunk;

  if (chunk->stream_discontinuity_event) {
    filter_reset(fs);
  }

  if (fs->apply_post_resample) {
    chunk->frames_to_write = filter_apply(fs, chunk, true);
  } else {
    chunk->frames_read = filter_apply(fs, chunk, false);
  }
  return chunk;
}

static void dsp_filter_cleanup(void *state) {
  filter_destroy((FilterState *)state);
}

static void dsp_filter_reset_api(void *state) {
  filter_reset((FilterState *)state);
}

static size_t dsp_filter_get_chunk_size(AppConfig *config) {
  size_t estimated_taps = 0;
  if (config->dsp.filter.args.taps > 0) {
    estimated_taps = config->dsp.filter.args.taps;
  } else if (config->dsp.filter.count > 0) {
    estimated_taps = FILTER_SAFETY_DEFAULT_TAPS;
  }

  size_t req_block_size = 0;
  if (estimated_taps > 0) {
    req_block_size = 1;
    while (req_block_size < estimated_taps) {
      req_block_size *= 2;
    }
    if (req_block_size < estimated_taps * 2) {
      req_block_size *= 2;
    }
  }
  return req_block_size;
}

static bool dsp_filter_is_active(AppContext *app, const char *stage_tag) {
  AppConfig *config = (AppConfig *)app->config;
  if (config->dsp.filter.count == 0) {
    return false;
  }
  if (stage_tag && strcmp(stage_tag, "pre") == 0) {
    return !config->dsp.filter.apply_post_resample;
  }
  if (stage_tag && strcmp(stage_tag, "post") == 0) {
    return config->dsp.filter.apply_post_resample;
  }
  return true;
}

static size_t dsp_filter_get_max_output_size(struct AppContext *app,
                                             size_t input_size) {
  AppConfig *config = (AppConfig *)app->config;
  return input_size + dsp_filter_get_chunk_size(config);
}

static const DspModuleInterface dsp_filter_api = {
    .name = "filter",
    .is_active = dsp_filter_is_active,
    .get_required_chunk_size = dsp_filter_get_chunk_size,
    .get_max_output_size = dsp_filter_get_max_output_size,
    .initialize = dsp_filter_init,
    .process = dsp_filter_process,
    .reset = dsp_filter_reset_api,
    .cleanup = dsp_filter_cleanup,
    .validate_options = dsp_filter_validate_options,
    .get_cli_options = dsp_filter_get_cli_options,
    .get_summary_info = filter_get_summary_info,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module m = {
      .name = "filter",
      .type = MODULE_TYPE_DSP,
      .api = (void *)&dsp_filter_api,
  };
  module_registry_add(&m);
}
