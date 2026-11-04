/**
 * @file presets_loader.c
 * @brief Configuration presets file parser and property loader.
 */

#include "presets_loader.h"
#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "mem_arena.h"
#include "platform.h"
#include "utilities.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- The Dispatch Table ---
static const PresetKeyHandler key_handlers[] = {
    {"description", PRESET_KEY_STRDUP, offsetof(PresetDefinition, description)},
    {"output_sample_rate", PRESET_KEY_STRTOD,
     offsetof(PresetDefinition, rate_hz)},
    {"baseband_sample_rate", PRESET_KEY_STRTOD,
     offsetof(PresetDefinition, baseband_sample_rate_hz)},
    {"output_sample_format", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, output_sample_format)},
    {"baseband_sample_format", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, baseband_sample_format)},
    {"input_gain_multiplier", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, input_gain)},
    {"output_gain_multiplier", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, output_gain)},
    {"baseband_gain_multiplier", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, baseband_gain)},
    {"dc_block", PRESET_KEY_BOOL, offsetof(PresetDefinition, dc_block_enable)},
    {"iq_correction", PRESET_KEY_BOOL,
     offsetof(PresetDefinition, iq_correction_enable)},
    {"output_agc", PRESET_KEY_BOOL,
     offsetof(PresetDefinition, output_agc_enable)},
    {"output_agc_target", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, output_agc_target)},
    {"baseband_agc", PRESET_KEY_BOOL,
     offsetof(PresetDefinition, baseband_agc_enable)},
    {"baseband_agc_target", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, baseband_agc_target)},
    {"freq_shift", PRESET_KEY_STRTOD,
     offsetof(PresetDefinition, frequency_shift_hz)},
    {"shift_after_resample", PRESET_KEY_BOOL,
     offsetof(PresetDefinition, shift_after_resample)},
    {"lowpass", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, lowpass_cutoff_hz[0])},
    {"lowpass_1", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, lowpass_cutoff_hz[0])},
    {"lowpass_2", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, lowpass_cutoff_hz[1])},
    {"lowpass_3", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, lowpass_cutoff_hz[2])},
    {"lowpass_4", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, lowpass_cutoff_hz[3])},
    {"lowpass_5", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, lowpass_cutoff_hz[4])},
    {"highpass", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, highpass_cutoff_hz[0])},
    {"highpass_1", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, highpass_cutoff_hz[0])},
    {"highpass_2", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, highpass_cutoff_hz[1])},
    {"highpass_3", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, highpass_cutoff_hz[2])},
    {"highpass_4", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, highpass_cutoff_hz[3])},
    {"highpass_5", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, highpass_cutoff_hz[4])},
    {"pass_range", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, pass_range_str[0])},
    {"pass_range_1", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, pass_range_str[0])},
    {"pass_range_2", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, pass_range_str[1])},
    {"pass_range_3", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, pass_range_str[2])},
    {"pass_range_4", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, pass_range_str[3])},
    {"pass_range_5", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, pass_range_str[4])},
    {"stopband", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, stopband_str[0])},
    {"stopband_1", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, stopband_str[0])},
    {"stopband_2", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, stopband_str[1])},
    {"stopband_3", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, stopband_str[2])},
    {"stopband_4", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, stopband_str[3])},
    {"stopband_5", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, stopband_str[4])},
    {"transition_width", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, transition_width_hz)},
    {"filter_taps", PRESET_KEY_STRTOL, offsetof(PresetDefinition, filter_taps)},
    {"attenuation", PRESET_KEY_STRTOF,
     offsetof(PresetDefinition, default_filter_attenuation_db)},
    {"filter_type", PRESET_KEY_STRDUP,
     offsetof(PresetDefinition, filter_type_str)},
    {"filter_fft_size", PRESET_KEY_STRTOL,
     offsetof(PresetDefinition, filter_fft_size)},
};
static const size_t num_key_handlers =
    sizeof(key_handlers) / sizeof(key_handlers[0]);

static bool preset_key_equals(const char *first_key, const char *second_key) {
  if (!first_key || !second_key) {
    return false;
  }
  while (*first_key != '\0' && *second_key != '\0') {
    char first_char =
        (*first_key == '-') ? '_' : (char)tolower((unsigned char)*first_key);
    char second_char =
        (*second_key == '-') ? '_' : (char)tolower((unsigned char)*second_key);
    if (first_char != second_char) {
      return false;
    }
    first_key++;
    second_key++;
  }
  return *first_key == '\0' && *second_key == '\0';
}

static char *arena_strdup(MemoryArena *arena, const char *source_string) {
  if (!source_string)
    return NULL;
  size_t length = strlen(source_string) + 1;
  char *copied_string = (char *)mem_arena_alloc(arena, length, false);
  if (copied_string) {
    memcpy(copied_string, source_string, length);
  }
  return copied_string;
}

bool presets_load_from_file(AppConfig *config, MemoryArena *arena) {
  config->presets = NULL;
  config->num_presets = 0;

  char full_path_buffer[APP_MAX_PATH_BUFFER];

  char *found_preset_files[5];
  int num_found_files = 0;

  const char *search_paths_list[10];
  size_t num_search_paths = platform_get_config_search_paths(
      search_paths_list,
      sizeof(search_paths_list) / sizeof(search_paths_list[0]), arena);

  for (size_t path_index = 0; path_index < num_search_paths; ++path_index) {
    const char *base_dir = search_paths_list[path_index];
    if (base_dir == NULL)
      continue;
    int written_chars = snprintf(full_path_buffer, sizeof(full_path_buffer),
                                 "%s/%s", base_dir, PRESETS_FILENAME);
    if (written_chars < 0 ||
        (size_t)written_chars >= sizeof(full_path_buffer)) {
      continue;
    }

    if (platform_is_file(full_path_buffer)) {
      if (num_found_files <
          (int)(sizeof(found_preset_files) / sizeof(found_preset_files[0]))) {
        found_preset_files[num_found_files] =
            arena_strdup(arena, full_path_buffer);
        if (!found_preset_files[num_found_files]) {
          return false;
        }
        num_found_files++;
      }
    }
  }

  if (num_found_files > 1) {
    log_warn("Conflicting presets files found. No presets will be loaded.");
    return true;
  } else if (num_found_files == 0) {
    log_info("No presets file '%s' found.", PRESETS_FILENAME);
    return true;
  }

  FILE *preset_file = platform_file_open_read(found_preset_files[0]);
  if (!preset_file) {
    log_error("Error opening presets file '%s': %s", found_preset_files[0],
              strerror(errno));
    return false;
  }

  PlatformFileStatus status = platform_file_verify(preset_file);
  if (status == PLATFORM_FILE_IS_DIRECTORY) {
    log_error("Security: Presets file '%s' is a directory. Aborting.",
              found_preset_files[0]);
    fclose(preset_file);
    return false;
  } else if (status != PLATFORM_FILE_OK) {
    log_fatal("Could not get file status for '%s'", found_preset_files[0]);
    fclose(preset_file);
    return false;
  }

  char line[PRESETS_MAX_LINE_LENGTH];
  PresetDefinition *current_preset = NULL;
  int capacity = 8;

  config->presets = (PresetDefinition *)mem_arena_alloc(
      arena, capacity * sizeof(PresetDefinition), true);
  if (!config->presets) {
    fclose(preset_file);
    return false;
  }

  int line_num = 0;
  while (fgets(line, sizeof(line), preset_file)) {
    line_num++;
    char *trimmed_line = utility_trim_whitespace(line);

    if (trimmed_line[0] == '#' || trimmed_line[0] == ';' ||
        trimmed_line[0] == '\0') {
      continue;
    }

    if (trimmed_line[0] == '[' && strstr(trimmed_line, "preset:")) {
      if (config->num_presets >= PRESETS_MAX_COUNT) {
        log_warn("Maximum number of presets (%d) reached at line %d. Ignoring "
                 "further presets.",
                 PRESETS_MAX_COUNT, line_num);
        current_preset = NULL;
        continue;
      }
      if (config->num_presets == capacity) {
        int old_capacity = capacity;
        capacity *= 2;
        PresetDefinition *new_presets = (PresetDefinition *)mem_arena_alloc(
            arena, capacity * sizeof(PresetDefinition), true);
        if (!new_presets) {
          fclose(preset_file);
          return false;
        }
        memcpy(new_presets, config->presets,
               old_capacity * sizeof(PresetDefinition));
        config->presets = new_presets;
      }
      current_preset = &config->presets[config->num_presets];
      memset(current_preset, 0, sizeof(PresetDefinition));
      char *name_start = trimmed_line + strlen("[preset:");
      char *name_end = strchr(name_start, ']');
      if (name_end) {
        *name_end = '\0';
        current_preset->name =
            arena_strdup(arena, utility_trim_whitespace(name_start));
        if (!current_preset->name) {
          fclose(preset_file);
          return false;
        }
        config->num_presets++;
      } else {
        log_warn("Malformed preset header at line %d: %s", line_num,
                 trimmed_line);
        current_preset = NULL;
      }
    } else if (current_preset && strchr(trimmed_line, '=')) {
      char *key = strtok(trimmed_line, "=");
      char *value = strtok(NULL, "");
      if (!key || !value) {
        log_warn("Malformed key-value pair at line %d.", line_num);
        continue;
      }
      key = utility_trim_whitespace(key);
      value = utility_trim_whitespace(value);

      bool key_found = false;
      for (size_t handler_index = 0; handler_index < num_key_handlers;
           ++handler_index) {
        const PresetKeyHandler *handler = &key_handlers[handler_index];
        if (preset_key_equals(key, handler->key_name)) {
          char *value_ptr = (char *)current_preset + handler->value_offset;

          switch (handler->action) {
          case PRESET_KEY_STRDUP:
            *(char **)value_ptr = arena_strdup(arena, value);
            if (!*(char **)value_ptr) {
              fclose(preset_file);
              return false;
            }
            break;
          case PRESET_KEY_STRTOD: {
            char *parse_end = NULL;
            double parsed_double = strtod(value, &parse_end);
            if (parse_end == value || *parse_end != '\0') {
              log_warn("Invalid numeric value '%s' for key '%s' in preset '%s' "
                       "at line %d.",
                       value, key, current_preset->name, line_num);
            } else {
              *(double *)value_ptr = parsed_double;
            }
            break;
          }
          case PRESET_KEY_STRTOF: {
            char *parse_end = NULL;
            float parsed_float = strtof(value, &parse_end);
            if (parse_end == value || *parse_end != '\0') {
              log_warn("Invalid numeric value '%s' for key '%s' in preset '%s' "
                       "at line %d.",
                       value, key, current_preset->name, line_num);
            } else {
              *(float *)value_ptr = parsed_float;
            }
            break;
          }
          case PRESET_KEY_STRTOL: {
            char *parse_end = NULL;
            long parsed_long = strtol(value, &parse_end, 10);
            if (parse_end == value || *parse_end != '\0') {
              log_warn("Invalid integer value '%s' for key '%s' in preset '%s' "
                       "at line %d.",
                       value, key, current_preset->name, line_num);
            } else {
              *(int *)value_ptr = (int)parsed_long;
            }
            break;
          }
          case PRESET_KEY_BOOL:
            if (strcasecmp(value, "true") == 0) {
              *(bool *)value_ptr = true;
            } else if (strcasecmp(value, "false") == 0) {
              *(bool *)value_ptr = false;
            } else {
              log_warn("Invalid boolean value '%s' for key '%s' in preset '%s' "
                       "at line %d (expected 'true' or 'false').",
                       value, key, current_preset->name, line_num);
            }
            break;
          }

          key_found = true;
          break;
        }
      }

      if (!key_found) {
        log_warn("Unknown key '%s' in preset '%s' at line %d.", key,
                 current_preset->name, line_num);
      }
    }
  }

  fclose(preset_file);
  return true;
}
