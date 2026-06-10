/**
 * @file presets_loader.h
 */

#ifndef PRESETS_LOADER_H_
#define PRESETS_LOADER_H_

#include <stddef.h>
#include <stdbool.h>
#include "common_types.h"

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
    const char*     key_name;
    PresetKeyAction action;
    size_t          value_offset;
    size_t          provided_flag_offset;
} PresetKeyHandler;

typedef struct PresetDefinition {
    char*  name;
    char*  description;
    double rate_hz;
    bool   rate_hz_provided;
    double baseband_sample_rate_hz;
    bool   baseband_sample_rate_provided;
    char*  output_sample_format;
    char*  baseband_sample_format;

    // DSP parameters
    float input_gain;
    bool  input_gain_provided;

    float output_gain;
    bool  output_gain_provided;

    float baseband_gain;
    bool  baseband_gain_provided;

    bool  dc_block_enable;
    bool  dc_block_provided;
    bool  iq_correction_enable;
    bool  iq_correction_provided;

    bool  output_agc_enable;
    bool  output_agc_enable_provided;
    float output_agc_target;
    bool  output_agc_target_provided;

    bool  baseband_agc_enable;
    bool  baseband_agc_enable_provided;
    float baseband_agc_target;
    bool  baseband_agc_target_provided;

    // Filter Fields
    float lowpass_cutoff_hz;
    bool  lowpass_cutoff_hz_provided;
    float highpass_cutoff_hz;
    bool  highpass_cutoff_hz_provided;
    char* pass_range_str;
    bool  pass_range_str_provided;
    char* stopband_str;
    bool  stopband_str_provided;
    float transition_width_hz;
    bool  transition_width_hz_provided;
    int   filter_taps;
    bool  filter_taps_provided;
    float default_filter_attenuation_db;
    bool  default_filter_attenuation_db_provided;
    char* filter_type_str;
    bool  filter_type_str_provided;
} PresetDefinition;

bool presets_load_from_file(struct AppConfig* config, struct MemoryArena* arena);

#endif // PRESETS_LOADER_H_
