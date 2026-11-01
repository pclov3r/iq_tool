/**
 * @file output/am.c
 * @brief AM Audio Demodulation and Output Module
 *
 * This module implements a robust AM demodulator with two modes:
 * 1. Synchronous AM (SYNC) using a Phase-Locked Loop (PLL) for high-fidelity
 * audio.
 * 2. Envelope AM (ENV) using complex magnitude detection.
 *
 * DSP ProcessChain (with references to Richard G. Lyons, "Understanding Digital
 * Signal Processing", 3rd Edition):
 * - Block Filtering: Uses liquid-dsp SIMD block execution for high-speed
 *   channel filtering, with tap coefficients normalized to exact unity (0 dB)
 *   passband DC gain (Lyons Section 5.10.1 Eq. 5-34 & Section 6.9).
 * - Demodulation: The PLL uses an explicit acquisition/tracking state machine
 *   and a four-quadrant atan2f(Q, I) carrier phase detector to lock
 *   unambiguously to 0-degree positive carrier phase. While acquiring lock on
 *   weak or off-frequency signals, it seamlessly outputs complex magnitude
 *   envelope audio (Lyons Section 9.2 Eq. 9-4) to prevent beat-note audio.
 * - DC Blocking: Carrier removal is performed post-demodulation using the
 *   scaled real-time IIR DC-removal filter (Lyons Section 13.23.2 Fig. 13-62(a)
 *   & Section 6.9).
 * - Carrier-Derived Leveling: Audio volume is normalized by the exponentially
 *   averaged carrier amplitude (Lyons Section 11.6 & Section 13.33.1
 *   Eq. 13-141') to recover true modulation depth without speech-pause noise
 *   pumping or audio-rate gain modulation distortion (Lyons Section 13.30).
 * - Resampling & Audio Filtering: SIMD block processing for audio cleanup.
 */

#include "app_context.h"
#include "audio_output_functions.h"
#include "config/constants.h"
#include "log.h"
#include "module.h"
#include "module_registry.h"
#include "platform.h"
#include "signal_handler.h"
#include "utilities.h"
#include <complex.h>
#include <liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Audio Output Constants ---
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define MAX_AUDIO_CUTOFF_HZ 20000.0f
#define CHANNEL_FILTER_TRANSITION_MARGIN_HZ 1000.0f
#define FIR_FILTER_TAP_LENGTH 63U

// --- Baseband Rate Constraints ---
#define AM_MIN_INPUT_RATE 12000.0
#define AM_MAX_INPUT_RATE 192000.0
#define AM_DEFAULT_INPUT_RATE 48000.0

// --- PLL & Demodulator Tuning Constants ---
#define AM_PLL_ACQUISITION_BANDWIDTH_HZ 100.0f
#define AM_PLL_TRACKING_BANDWIDTH_HZ 15.0f
#define AM_PLL_MAX_FREQUENCY_OFFSET_HZ 200.0f
#define AM_PLL_LOCK_SNR_THRESHOLD_DB 10.0f
#define AM_PLL_UNLOCK_SNR_THRESHOLD_DB 6.0f
#define AM_SIGNAL_LOSS_THRESHOLD_DBFS -70.0f
#define AM_LOCK_TIMEOUT_INTERVALS 3

// --- DC Blocker & Carrier Envelope Tracker Constants ---
#define AM_DC_BLOCK_CUTOFF_HZ 20.0f
#define AM_CARRIER_TRACKING_CUTOFF_HZ 2.0f
#define AM_MIN_CARRIER_ENVELOPE 0.001f

typedef enum {
  AM_PLL_STATE_ENVELOPE_FORCED = 0,
  AM_PLL_STATE_ACQUIRING,
  AM_PLL_STATE_LOCKED
} AmPllState;

// --- Internal Structures ---
typedef struct {
  // ProcessChain State
  AudioOutputContext *audio_output;

  // DSP Objects (Liquid)
  firfilt_crcf channel_filter;
  nco_crcf carrier_pll;
  firfilt_rrrf audio_lowpass_filter;
  msresamp_rrrf audio_resampler;

  // Scaled Real-Time DC Blocker State (Lyons Section 13.23.2 Fig. 13-62(a))
  float dc_block_previous_input;
  float dc_block_previous_output;
  float dc_block_pole_alpha;
  float dc_block_feedforward_scale;

  // Carrier Amplitude Envelope Tracker (Lyons Section 13.33.1 & 13.33.3)
  float carrier_amplitude_estimate;
  float carrier_tracking_alpha;
  unsigned int filter_warmup_samples_remaining;
  size_t carrier_averager_sample_count;

  // Processing & PLL State Machine
  float input_sample_rate;
  float max_pll_frequency_radians;
  float manual_gain;
  AmPllState pll_state;
  int unlock_timeout_count;

  // Console Statistics & Carrier Lock Detector (Lyons Section 13.33.1 & App. D.5)
  float lock_detector_quadrature_filter;
  float lock_detector_filter_alpha;
  size_t statistics_sample_counter;
  size_t statistics_sample_threshold;
  double accumulated_magnitude_squared_sum;
  double accumulated_carrier_frequency_sum;
  double accumulated_in_phase_sum;
  double accumulated_quadrature_squared_sum;
  size_t accumulated_pll_sample_count;

  // Scratch Buffers for SIMD Block Processing
  liquid_float_complex *filtered_baseband_buffer;
  float *mono_audio_buffer;
  float *resampled_audio_buffer;
  float *final_audio_buffer;
  int16_t *interleaved_pcm_buffer;
} AmContext;

// --- CLI Config ---
static struct {
  float audio_gain;
  float audio_cutoff_hz;
  bool force_envelope;
} s_am_config = {
    .audio_gain = 1.0f, .audio_cutoff_hz = 5000.0f, .force_envelope = false};

/**
 * @brief Normalizes FIR filter tap coefficients to exact unity (0 dB) DC gain.
 *
 * Per Richard G. Lyons, "Understanding Digital Signal Processing", 3rd ed.,
 * Section 5.10.1 (Eq. 5-34) and Section 6.9 (p. 276), the DC gain of an FIR
 * filter equals the sum of its impulse response coefficients. Dividing each
 * coefficient by that sum enforces exact unity passband gain at 0 Hz.
 */
static void normalize_fir_filter_dc_gain(float *coefficients,
                                         unsigned int filter_length) {
  double coefficient_sum = 0.0;
  for (unsigned int tap_index = 0; tap_index < filter_length; tap_index++) {
    coefficient_sum += (double)coefficients[tap_index];
  }
  if (fabs(coefficient_sum) > 1e-12) {
    float normalization_scale = (float)(1.0 / coefficient_sum);
    for (unsigned int tap_index = 0; tap_index < filter_length; tap_index++) {
      coefficients[tap_index] *= normalization_scale;
    }
  }
}

// --- Module Interface Implementation ---

static bool output_am_validate_options(AppContext *app) {
  AppConfig *config = app ? (AppConfig *)app->config : NULL;
  if (!config) {
    return false;
  }
  config->baseband_sample_format.format = CF32;

  if (config->baseband_sample_rate.rate_hz == 0.0) {
    config->baseband_sample_rate.rate_hz = AM_DEFAULT_INPUT_RATE;
    config->baseband_sample_rate.provided = true;
  } else {
    double sample_rate = config->baseband_sample_rate.rate_hz;
    if (sample_rate < AM_MIN_INPUT_RATE) {
      log_error("AM: Input rate %.15g Hz is too low for audio (Min: %.15g).",
                sample_rate, AM_MIN_INPUT_RATE);
      return false;
    }
    if (sample_rate > AM_MAX_INPUT_RATE) {
      log_error("AM: Input rate %.15g Hz is unnecessarily high (Max: %.15g).",
                sample_rate, AM_MAX_INPUT_RATE);
      return false;
    }
  }
  return true;
}

static bool output_am_initialize(ModuleContext *context) {
  AppContext *app = context->app;

  AmContext *am_context = (AmContext *)mem_arena_alloc(
      &app->process_chain.setup_arena, sizeof(AmContext), true);
  if (!am_context) {
    return false;
  }
  app->module.output_private_data = am_context;

  am_context->audio_output =
      audio_output_create(app, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                          app->module.input_info.demod_audio_buffer_size);
  if (!am_context->audio_output) {
    return false;
  }

  // 1. Base DSP Configuration
  float input_sample_rate = (float)context->config->baseband_sample_rate.rate_hz;
  if (input_sample_rate < 1.0f) {
    input_sample_rate = (float)AM_DEFAULT_INPUT_RATE;
  }

  am_context->input_sample_rate = input_sample_rate;
  am_context->max_pll_frequency_radians =
      (2.0f * (float)M_PI * AM_PLL_MAX_FREQUENCY_OFFSET_HZ) / input_sample_rate;
  am_context->manual_gain = s_am_config.audio_gain;

  if (s_am_config.audio_cutoff_hz > MAX_AUDIO_CUTOFF_HZ) {
    s_am_config.audio_cutoff_hz = MAX_AUDIO_CUTOFF_HZ;
  }

  // Initialize PLL State Machine & Statistics
  am_context->pll_state = s_am_config.force_envelope
                              ? AM_PLL_STATE_ENVELOPE_FORCED
                              : AM_PLL_STATE_ACQUIRING;
  am_context->unlock_timeout_count = 0;

  am_context->lock_detector_quadrature_filter = 0.0f;
  am_context->lock_detector_filter_alpha =
      1.0f - expf(-2.0f * (float)M_PI * AM_PLL_TRACKING_BANDWIDTH_HZ /
                  input_sample_rate);
  am_context->statistics_sample_counter = 0;
  am_context->accumulated_magnitude_squared_sum = 0.0;
  am_context->accumulated_carrier_frequency_sum = 0.0;
  am_context->accumulated_in_phase_sum = 0.0;
  am_context->accumulated_quadrature_squared_sum = 0.0;
  am_context->accumulated_pll_sample_count = 0;
  am_context->statistics_sample_threshold =
      (size_t)(input_sample_rate * CONSOLE_UPDATE_INTERVAL_SEC);

  log_info("AM: Baseband %.15g Hz | Audio Cutoff: %.15g Hz | Mode: %s",
           input_sample_rate, s_am_config.audio_cutoff_hz,
           (am_context->pll_state == AM_PLL_STATE_ENVELOPE_FORCED)
               ? "Envelope (Magnitude)"
               : "Synchronous (PLL)");

  // 2. A. Baseband Channel Filter (Normalized to Unity Passband Gain)
  float channel_filter_coefficients[FIR_FILTER_TAP_LENGTH];
  float normalized_channel_cutoff =
      (s_am_config.audio_cutoff_hz + CHANNEL_FILTER_TRANSITION_MARGIN_HZ) /
      input_sample_rate;
  if (normalized_channel_cutoff > 0.49f) {
    normalized_channel_cutoff = 0.49f;
  }
  liquid_firdes_kaiser(FIR_FILTER_TAP_LENGTH, normalized_channel_cutoff,
                       context->config->dsp.filter.args.attenuation, 0.0f,
                       channel_filter_coefficients);
  normalize_fir_filter_dc_gain(channel_filter_coefficients,
                               FIR_FILTER_TAP_LENGTH);
  am_context->channel_filter =
      firfilt_crcf_create(channel_filter_coefficients, FIR_FILTER_TAP_LENGTH);

  // 2. B. Demodulator (PLL)
  if (am_context->pll_state != AM_PLL_STATE_ENVELOPE_FORCED) {
    am_context->carrier_pll = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(am_context->carrier_pll,
                               AM_PLL_ACQUISITION_BANDWIDTH_HZ /
                                   input_sample_rate);
  } else {
    am_context->carrier_pll = NULL;
  }

  // 2. C. Scaled Real-Time DC Blocker (Lyons Section 13.23.2 Fig. 13-62(a))
  am_context->dc_block_pole_alpha =
      1.0f -
      (2.0f * (float)M_PI * AM_DC_BLOCK_CUTOFF_HZ / input_sample_rate);
  if (am_context->dc_block_pole_alpha < 0.8f) {
    am_context->dc_block_pole_alpha = 0.8f;
  }
  am_context->dc_block_feedforward_scale =
      (1.0f + am_context->dc_block_pole_alpha) * 0.5f;
  am_context->dc_block_previous_input = 0.0f;
  am_context->dc_block_previous_output = 0.0f;

  // 2. D. Carrier Amplitude Envelope Tracker (Lyons Section 13.33.1 & 13.33.3)
  am_context->carrier_tracking_alpha =
      1.0f - expf(-2.0f * (float)M_PI * AM_CARRIER_TRACKING_CUTOFF_HZ /
                  input_sample_rate);
  am_context->carrier_amplitude_estimate = AM_MIN_CARRIER_ENVELOPE;
  am_context->filter_warmup_samples_remaining = FIR_FILTER_TAP_LENGTH;
  am_context->carrier_averager_sample_count = 0;

  // 2. E. Resampler
  float output_resample_ratio = (float)AUDIO_SAMPLE_RATE / input_sample_rate;
  am_context->audio_resampler = msresamp_rrrf_create(
      output_resample_ratio, context->config->dsp.filter.args.attenuation);

  // 2. F. Audio Lowpass Filter (Normalized to Unity Passband Gain)
  float audio_filter_coefficients[FIR_FILTER_TAP_LENGTH];
  float normalized_audio_cutoff =
      s_am_config.audio_cutoff_hz / (float)AUDIO_SAMPLE_RATE;
  if (normalized_audio_cutoff > 0.49f) {
    normalized_audio_cutoff = 0.49f;
  }
  liquid_firdes_kaiser(FIR_FILTER_TAP_LENGTH, normalized_audio_cutoff,
                       context->config->dsp.filter.args.attenuation, 0.0f,
                       audio_filter_coefficients);
  normalize_fir_filter_dc_gain(audio_filter_coefficients,
                               FIR_FILTER_TAP_LENGTH);
  am_context->audio_lowpass_filter =
      firfilt_rrrf_create(audio_filter_coefficients, FIR_FILTER_TAP_LENGTH);

  // 3. Scratch Buffers for Block Processing
  size_t input_buffer_samples = app->process_chain.alloc_size_samples;
  size_t output_buffer_samples =
      (size_t)ceil((double)input_buffer_samples * (double)output_resample_ratio) +
      128U;

  am_context->filtered_baseband_buffer = mem_arena_alloc(
      &app->process_chain.setup_arena,
      input_buffer_samples * sizeof(liquid_float_complex), false);
  am_context->mono_audio_buffer =
      mem_arena_alloc(&app->process_chain.setup_arena,
                      input_buffer_samples * sizeof(float), false);
  am_context->resampled_audio_buffer =
      mem_arena_alloc(&app->process_chain.setup_arena,
                      output_buffer_samples * sizeof(float), false);
  am_context->final_audio_buffer =
      mem_arena_alloc(&app->process_chain.setup_arena,
                      output_buffer_samples * sizeof(float), false);
  am_context->interleaved_pcm_buffer =
      mem_arena_alloc(&app->process_chain.setup_arena,
                      output_buffer_samples * AUDIO_CHANNELS * sizeof(int16_t),
                      false);

  if (!am_context->filtered_baseband_buffer || !am_context->mono_audio_buffer ||
      !am_context->resampled_audio_buffer || !am_context->final_audio_buffer ||
      !am_context->interleaved_pcm_buffer) {
    return false;
  }

  return true;
}

static void output_am_reset(ModuleContext *context) {
  if (!context || !context->app || !context->app->module.output_private_data) {
    return;
  }
  AmContext *am_context =
      (AmContext *)context->app->module.output_private_data;

  if (am_context->channel_filter) {
    firfilt_crcf_reset(am_context->channel_filter);
  }
  if (am_context->carrier_pll) {
    nco_crcf_reset(am_context->carrier_pll);
    nco_crcf_pll_set_bandwidth(am_context->carrier_pll,
                               AM_PLL_ACQUISITION_BANDWIDTH_HZ /
                                   am_context->input_sample_rate);
  }
  if (am_context->audio_resampler) {
    msresamp_rrrf_reset(am_context->audio_resampler);
  }
  if (am_context->audio_lowpass_filter) {
    firfilt_rrrf_reset(am_context->audio_lowpass_filter);
  }

  am_context->dc_block_previous_input = 0.0f;
  am_context->dc_block_previous_output = 0.0f;
  am_context->carrier_amplitude_estimate = AM_MIN_CARRIER_ENVELOPE;
  am_context->filter_warmup_samples_remaining = FIR_FILTER_TAP_LENGTH;
  am_context->carrier_averager_sample_count = 0;
  am_context->lock_detector_quadrature_filter = 0.0f;

  if (am_context->pll_state != AM_PLL_STATE_ENVELOPE_FORCED) {
    am_context->pll_state = AM_PLL_STATE_ACQUIRING;
  }
  am_context->unlock_timeout_count = 0;
}

static void output_am_flush(ModuleContext *context) {
  AmContext *am_context = (AmContext *)context->app->module.output_private_data;
  if (is_shutdown_requested()) {
    audio_output_clear(am_context->audio_output);
  } else {
    audio_output_drain(am_context->audio_output);
  }
}

static size_t output_am_write_chunk(ModuleContext *context, const void *buffer,
                                    size_t input_bytes) {
  AppContext *app = context->app;
  AmContext *am_context = (AmContext *)app->module.output_private_data;

  const float pcm_full_scale = 32767.0f;

  if (input_bytes == 0) {
    return 0;
  }
  unsigned int frame_count =
      (unsigned int)(input_bytes / app->module.output_bytes_per_iq_sample);
  const liquid_float_complex *input_iq_samples =
      (const liquid_float_complex *)buffer;

  // 1. SIMD Block Processing: Baseband Channel Filter
  firfilt_crcf_execute_block(am_context->channel_filter,
                             (liquid_float_complex *)input_iq_samples,
                             frame_count, am_context->filtered_baseband_buffer);

  // 2. Demodulation, DC Removal, and Carrier-Derived Normalization Loop
  for (unsigned int frame_index = 0; frame_index < frame_count; frame_index++) {
    liquid_float_complex filtered_iq_sample =
        am_context->filtered_baseband_buffer[frame_index];
    float envelope_magnitude = cabsf(filtered_iq_sample);
    float raw_envelope;

    if (am_context->pll_state != AM_PLL_STATE_ENVELOPE_FORCED) {
      liquid_float_complex local_carrier;
      nco_crcf_cexpf(am_context->carrier_pll, &local_carrier);
      liquid_float_complex demodulated_product =
          filtered_iq_sample * conjf(local_carrier);

      float in_phase_product = crealf(demodulated_product);
      float quadrature_product = cimagf(demodulated_product);

      float phase_detector_reference =
          (am_context->pll_state == AM_PLL_STATE_LOCKED)
              ? fmaxf(am_context->carrier_amplitude_estimate,
                      AM_MIN_CARRIER_ENVELOPE)
              : in_phase_product;
      float phase_error =
          atan2f(quadrature_product, phase_detector_reference);
      nco_crcf_pll_step(am_context->carrier_pll, phase_error);
      float pll_frequency_radians =
          nco_crcf_get_frequency(am_context->carrier_pll);
      if (pll_frequency_radians > am_context->max_pll_frequency_radians) {
        nco_crcf_set_frequency(am_context->carrier_pll,
                               am_context->max_pll_frequency_radians);
      } else if (pll_frequency_radians <
                 -am_context->max_pll_frequency_radians) {
        nco_crcf_set_frequency(am_context->carrier_pll,
                               -am_context->max_pll_frequency_radians);
      }
      nco_crcf_step(am_context->carrier_pll);

      if (am_context->pll_state == AM_PLL_STATE_LOCKED) {
        raw_envelope = in_phase_product;
      } else {
        raw_envelope = envelope_magnitude;
      }

      // Lowpass filter the quadrature branch to the PLL tracking bandwidth
      // (Lyons Section 13.33.1 Eq. 13-141') before squaring for lock SNR
      // estimation so asymmetric audio sidebands during selective fading are
      // not mistaken for carrier unlock noise.
      am_context->lock_detector_quadrature_filter +=
          am_context->lock_detector_filter_alpha *
          (quadrature_product - am_context->lock_detector_quadrature_filter);

      if ((frame_index & 3U) == 0U) {
        am_context->accumulated_carrier_frequency_sum +=
            (double)((nco_crcf_get_frequency(am_context->carrier_pll) /
                      (2.0f * (float)M_PI)) *
                     am_context->input_sample_rate);
        am_context->accumulated_in_phase_sum += (double)in_phase_product;
        am_context->accumulated_quadrature_squared_sum +=
            (double)(am_context->lock_detector_quadrature_filter *
                     am_context->lock_detector_quadrature_filter);
        am_context->accumulated_pll_sample_count++;
      }
    } else {
      raw_envelope = envelope_magnitude;
    }

    am_context->accumulated_magnitude_squared_sum +=
        (double)(envelope_magnitude * envelope_magnitude);

    // Wait for the FIR channel filter delay line to fill before seeding the
    // DC blocker state (Lyons Section 5.10.3 & Figure 13-64) to prevent initial
    // step-response transients.
    if (am_context->filter_warmup_samples_remaining > 0U) {
      am_context->filter_warmup_samples_remaining--;
      if (am_context->filter_warmup_samples_remaining == 0U) {
        am_context->dc_block_previous_input = raw_envelope;
        am_context->dc_block_previous_output = 0.0f;
      }
      am_context->mono_audio_buffer[frame_index] = 0.0f;
      continue;
    }

    // Dual-mode single-multiply exponential averager tracking the carrier DC
    // amplitude (Lyons Section 13.33.1 Eq. 13-141' & Section 13.33.3
    // Fig. 13-81(e)): uses exact 1/K running mean during initial convergence
    // until 1/K <= carrier_tracking_alpha, then switches to steady-state EMA.
    am_context->carrier_averager_sample_count++;
    float reciprocal_sample_count =
        1.0f / (float)am_context->carrier_averager_sample_count;
    float effective_tracking_alpha =
        fmaxf(am_context->carrier_tracking_alpha, reciprocal_sample_count);
    am_context->carrier_amplitude_estimate +=
        effective_tracking_alpha *
        (envelope_magnitude - am_context->carrier_amplitude_estimate);

    // Scaled Real-Time DC Blocker (Lyons Section 13.23.2 Fig. 13-62(a))
    float dc_blocked_audio =
        (am_context->dc_block_feedforward_scale *
         (raw_envelope - am_context->dc_block_previous_input)) +
        (am_context->dc_block_pole_alpha *
         am_context->dc_block_previous_output);
    am_context->dc_block_previous_input = raw_envelope;
    am_context->dc_block_previous_output = dc_blocked_audio;

    // Normalize by carrier amplitude to recover modulation depth m(t)
    float safe_carrier_amplitude =
        fmaxf(am_context->carrier_amplitude_estimate, AM_MIN_CARRIER_ENVELOPE);
    am_context->mono_audio_buffer[frame_index] =
        dc_blocked_audio / safe_carrier_amplitude;
  }
  am_context->statistics_sample_counter += frame_count;

  // --- Periodic Statistics Logging, Coherent SNR Check, & PLL State Machine ---
  if (am_context->statistics_sample_counter >=
      am_context->statistics_sample_threshold) {
    float dbfs = utility_calculate_dbfs(
        (float)(am_context->accumulated_magnitude_squared_sum /
                (double)am_context->statistics_sample_counter));

    if (am_context->pll_state != AM_PLL_STATE_ENVELOPE_FORCED &&
        am_context->accumulated_pll_sample_count > 0) {
      float mean_in_phase =
          (float)(am_context->accumulated_in_phase_sum /
                  (double)am_context->accumulated_pll_sample_count);
      float carrier_power =
          (mean_in_phase > 0.0f) ? (mean_in_phase * mean_in_phase) : 0.0f;
      float quadrature_noise_power =
          (float)(am_context->accumulated_quadrature_squared_sum /
                  (double)am_context->accumulated_pll_sample_count);

      float snr_db = 0.0f;
      if (quadrature_noise_power > 1e-12f) {
        snr_db = 10.0f * log10f(fmaxf(carrier_power, 1e-12f) /
                                quadrature_noise_power);
      } else if (carrier_power > 0.0f) {
        snr_db = 99.9f;
      }

      if (dbfs < AM_SIGNAL_LOSS_THRESHOLD_DBFS) {
        if (am_context->pll_state == AM_PLL_STATE_LOCKED) {
          am_context->pll_state = AM_PLL_STATE_ACQUIRING;
          nco_crcf_pll_set_bandwidth(am_context->carrier_pll,
                                     AM_PLL_ACQUISITION_BANDWIDTH_HZ /
                                         am_context->input_sample_rate);
        }
        nco_crcf_set_frequency(am_context->carrier_pll, 0.0f);
        am_context->unlock_timeout_count = 0;
      } else if (am_context->pll_state == AM_PLL_STATE_LOCKED) {
        if (mean_in_phase <= 0.0f || snr_db < AM_PLL_UNLOCK_SNR_THRESHOLD_DB) {
          if (++am_context->unlock_timeout_count >= AM_LOCK_TIMEOUT_INTERVALS) {
            log_warn("AM: PLL lost lock. Falling back to Envelope detection.");
            am_context->pll_state = AM_PLL_STATE_ACQUIRING;
            am_context->unlock_timeout_count = 0;
            nco_crcf_set_frequency(am_context->carrier_pll, 0.0f);
            nco_crcf_pll_set_bandwidth(am_context->carrier_pll,
                                       AM_PLL_ACQUISITION_BANDWIDTH_HZ /
                                           am_context->input_sample_rate);
          }
        } else {
          am_context->unlock_timeout_count = 0;
        }
      } else if (am_context->pll_state == AM_PLL_STATE_ACQUIRING) {
        if (mean_in_phase > 0.0f && snr_db >= AM_PLL_LOCK_SNR_THRESHOLD_DB) {
          log_info("AM: PLL locked to carrier.");
          am_context->pll_state = AM_PLL_STATE_LOCKED;
          am_context->unlock_timeout_count = 0;
          nco_crcf_pll_set_bandwidth(am_context->carrier_pll,
                                     AM_PLL_TRACKING_BANDWIDTH_HZ /
                                         am_context->input_sample_rate);
        }
      }

      const char *state_label =
          (am_context->pll_state == AM_PLL_STATE_LOCKED)
              ? "SYNC PLL LOCKED"
              : "ENVELOPE (ACQUIRING PLL)";

      log_info("dBFS: %.1f | SNR: %.1f dB | Offset: %.2f Hz [%s]", dbfs, snr_db,
               (float)(am_context->accumulated_carrier_frequency_sum /
                       (double)am_context->accumulated_pll_sample_count),
               state_label);

      am_context->accumulated_carrier_frequency_sum = 0.0;
      am_context->accumulated_in_phase_sum = 0.0;
      am_context->accumulated_quadrature_squared_sum = 0.0;
      am_context->accumulated_pll_sample_count = 0;
    } else {
      log_info("dBFS: %.1f [ENVELOPE]", dbfs);
    }

    am_context->statistics_sample_counter = 0;
    am_context->accumulated_magnitude_squared_sum = 0.0;
  }

  // 3. SIMD Block Processing: Resampler
  unsigned int resampled_frame_count = 0;
  msresamp_rrrf_execute(am_context->audio_resampler,
                        am_context->mono_audio_buffer, frame_count,
                        am_context->resampled_audio_buffer,
                        &resampled_frame_count);

  // 4. SIMD Block Processing: Audio Lowpass Filter
  firfilt_rrrf_execute_block(am_context->audio_lowpass_filter,
                             am_context->resampled_audio_buffer,
                             resampled_frame_count,
                             am_context->final_audio_buffer);

  // 5. Output Gain Scaling & Stereo PCM Interleaving
  // A 0.5 headroom factor gives 100% AM modulation (m = 1.0) 6 dB of headroom
  // at default gain (1.0), accommodating positive modulation peaks up to 200%.
  const float output_gain_scale = 0.5f * am_context->manual_gain;

  for (unsigned int sample_index = 0; sample_index < resampled_frame_count;
       sample_index++) {
    float scaled_sample =
        am_context->final_audio_buffer[sample_index] * output_gain_scale;
    float clipped_sample = fmaxf(-1.0f, fminf(1.0f, scaled_sample));

    int16_t pcm_sample = (int16_t)(clipped_sample * pcm_full_scale);
    am_context->interleaved_pcm_buffer[2U * sample_index] = pcm_sample;
    am_context->interleaved_pcm_buffer[2U * sample_index + 1U] = pcm_sample;
  }

  audio_output_write(am_context->audio_output,
                     am_context->interleaved_pcm_buffer,
                     resampled_frame_count * AUDIO_CHANNELS * sizeof(int16_t),
                     app->process_chain_mode);
  return input_bytes;
}

static void output_am_cleanup(ModuleContext *context) {
  AppContext *app = context->app;
  if (!app->module.output_private_data) {
    return;
  }
  AmContext *am_context = (AmContext *)app->module.output_private_data;

  audio_output_destroy(am_context->audio_output);

  if (am_context->channel_filter) {
    firfilt_crcf_destroy(am_context->channel_filter);
  }
  if (am_context->carrier_pll) {
    nco_crcf_destroy(am_context->carrier_pll);
  }
  if (am_context->audio_lowpass_filter) {
    firfilt_rrrf_destroy(am_context->audio_lowpass_filter);
  }
  if (am_context->audio_resampler) {
    msresamp_rrrf_destroy(am_context->audio_resampler);
  }
}

static void output_am_get_summary_info(const ModuleContext *context,
                                       SummaryInfo *info) {
  (void)context;
  utility_add_summary_item(info, "Output Type", "AM Audio");
  utility_add_summary_item(info, "Mode", "%s",
                           s_am_config.force_envelope ? "Envelope (Mag)"
                                                      : "Synchronous (PLL)");
  utility_add_summary_item(info, "Audio Sample Rate", "%d Hz",
                           AUDIO_SAMPLE_RATE);
  utility_add_summary_item(info, "Filter Cutoff", "%.15g Hz",
                           s_am_config.audio_cutoff_hz);
}

// clang-format off
static const struct argparse_option output_am_cli_options[] = {
        OPT_GROUP("AM Output (am)"),
        OPT_FLOAT(0, "am-gain", &s_am_config.audio_gain, "Set audio output gain (linear).", NULL, 0, 0),
        OPT_FLOAT(0, "am-cutoff", &s_am_config.audio_cutoff_hz, "Set audio lowpass filter cutoff in Hz (default: 5000).", NULL, 0, 0),
        OPT_BOOLEAN( 0, "am-envelope", &s_am_config.force_envelope, "Disable Synchronous AM (PLL) and use Magnitude Envelope Detection.", NULL, 0, 0),
};
// clang-format on

static const struct argparse_option *output_am_get_cli_options(int *count) {
  *count = sizeof(output_am_cli_options) / sizeof(output_am_cli_options[0]);
  return output_am_cli_options;
}

static void output_am_get_pipeline_requirements(const struct AppConfig *config,
                                                double *rate,
                                                SampleFormat *format,
                                                float *gain,
                                                struct OutputAgcConfig *agc) {
  *rate = config->baseband_sample_rate.rate_hz;
  *format = config->baseband_sample_format.format;
  *gain = config->dsp.baseband_gain;
  *agc = config->dsp.baseband_agc;
}

static OutputModuleInterface s_output_am_api = {
    .get_pipeline_requirements = output_am_get_pipeline_requirements,
    .initialize = output_am_initialize,
    .write_chunk = output_am_write_chunk,
    .reset = output_am_reset,
    .flush = output_am_flush,
    .cleanup = output_am_cleanup,
    .get_summary_info = output_am_get_summary_info,
    .validate_options = output_am_validate_options,
};

// --- Auto-Registration ---
static void __attribute__((constructor)) register_module(void) {
  Module am_module = {
      .name = "am",
      .type = MODULE_TYPE_OUTPUT,
      .payload = PAYLOAD_AUDIO,
      .api = (void *)&s_output_am_api,
      .set_default_config = NULL,
      .get_cli_options = output_am_get_cli_options,
      .requires_input_path = false,
      .requires_output_path = false,
      .module_defines_format = true,
  };
  module_registry_add(&am_module);
}
