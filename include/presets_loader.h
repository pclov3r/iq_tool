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
    double target_rate;
    char*  output_sample_format_name;

    // DSP parameters
    float gain;
    bool  gain_provided;
    bool  dc_block_enable;
    bool  dc_block_provided;
    bool  iq_correction_enable;
    bool  iq_correction_provided;

    // --- AGC Parameters ---
    char* agc_profile_str;
    bool  agc_profile_provided;
    float agc_target;
    bool  agc_target_provided;

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
    float attenuation_db;
    bool  attenuation_db_provided;
    char* filter_type_str;
    bool  filter_type_str_provided;
} PresetDefinition;

bool presets_load_from_file(struct AppConfig* config, struct MemoryArena* arena);

#endif // PRESETS_LOADER_H_
