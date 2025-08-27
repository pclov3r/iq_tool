/**
 * @file presets_loader.h
 * @brief Defines the interface for loading and parsing user-defined presets.
 *
 * This module is responsible for finding and parsing the presets configuration
 * file (e.g., 'iq_resample_tool_presets.conf'). It defines the data structures
 * that hold a single preset's configuration and the internal types used by the
 * parser's dispatch table.
 */

#ifndef PRESETS_LOADER_H_
#define PRESETS_LOADER_H_

#include <stddef.h>
#include "common_types.h" // For OutputType enum

// --- Forward Declarations ---
struct AppConfig;
struct MemoryArena;

// --- Type Definitions ---

/**
 * @enum PresetKeyAction
 * @brief Defines the action the parser should take for a given key.
 *
 * This is used in a dispatch table to map key names from the config file
 * to specific parsing functions (e.g., parse as string, parse as float).
 */
typedef enum {
    PRESET_KEY_STRDUP,
    PRESET_KEY_STRTOD,
    PRESET_KEY_STRTOF,
    PRESET_KEY_STRTOL,
    PRESET_KEY_BOOL,
    PRESET_KEY_OUTPUT_TYPE
} PresetKeyAction;

/**
 * @struct PresetKeyHandler
 * @brief Maps a key string from the config file to a parsing action and a destination offset.
 *
 * This structure is the core of the parser's dispatch table, allowing for a
 * flexible and easily extensible preset file format.
 */
typedef struct {
    const char*     key_name;               ///< The key string to match in the file (e.g., "target_rate").
    PresetKeyAction action;                 ///< The parsing action to perform on the value.
    size_t          value_offset;           ///< The byte offset of the target field within PresetDefinition.
    size_t          provided_flag_offset;   ///< The byte offset of the corresponding "_provided" boolean flag.
} PresetKeyHandler;

/**
 * @struct PresetDefinition
 * @brief Holds all the configuration values for a single named preset.
 */
typedef struct PresetDefinition {
    char*  name;
    char*  description;
    double target_rate;
    char*  sample_format_name;
    OutputType output_type;

    // DSP parameters with flags to check if they were set by the preset
    float gain;
    bool  gain_provided;
    bool  dc_block_enable;
    bool  dc_block_provided;
    bool  iq_correction_enable;
    bool  iq_correction_provided;

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


// --- Function Declarations ---

/**
 * @brief Loads preset definitions from a text file, searching common system locations.
 *
 * This function searches for the presets file in platform-specific user and system
 * configuration directories. If found, it parses the file and populates the
 * `presets` array within the AppConfig struct. All memory for the presets is
 * allocated from the provided memory arena.
 *
 * @param config A pointer to the AppConfig struct where the loaded presets will be stored.
 * @param arena A pointer to the memory arena to use for all allocations.
 * @return true on success (even if no file is found), false on a fatal error (e.g., file open/parse error).
 */
bool presets_load_from_file(struct AppConfig* config, struct MemoryArena* arena);

#endif // PRESETS_LOADER_H_
