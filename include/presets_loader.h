/**
 * @file presets_loader.h
 * @brief Defines data structures and functions for loading application presets.
 */

#ifndef PRESETS_LOADER_H_
#define PRESETS_LOADER_H_

#include "config/constants.h"
#include <stdbool.h>
#include <stddef.h>

// --- Forward Declarations ---
struct AppConfig;
struct MemoryArena;

// --- Type Definitions ---

typedef enum {
  PRESET_KEY_STRDUP,
  PRESET_KEY_STRTOD,
  PRESET_KEY_STRTOF,
  PRESET_KEY_STRTOL,
  PRESET_KEY_BOOL,
} PresetKeyAction;

typedef struct {
  const char *key_name;
  PresetKeyAction action;
  size_t value_offset;
} PresetKeyHandler;

typedef struct PresetDefinition {
  char *name;
  char *description;
  double rate_hz;
  double baseband_sample_rate_hz;
  char *output_sample_format;
  char *baseband_sample_format;

  // DSP parameters
  float input_gain;
  float output_gain;
  float baseband_gain;

  bool dc_block_enable;
  bool iq_correction_enable;

  bool output_agc_enable;
  float output_agc_target;

  bool baseband_agc_enable;
  float baseband_agc_target;

  double frequency_shift_hz;
  bool shift_after_resample;

  // Filter Fields
  float lowpass_cutoff_hz[FILTER_MAX_CHAIN];
  float highpass_cutoff_hz[FILTER_MAX_CHAIN];
  char *pass_range_str[FILTER_MAX_CHAIN];
  char *stopband_str[FILTER_MAX_CHAIN];
  float transition_width_hz;
  int filter_taps;
  float default_filter_attenuation_db;
  char *filter_type_str;
  int filter_fft_size;
} PresetDefinition;

bool presets_load_from_file(struct AppConfig *config,
                            struct MemoryArena *arena);

#endif // PRESETS_LOADER_H_
