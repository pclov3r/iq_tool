/**
 * @file cli.c
 * @brief Command-line argument parsing and configuration validation.
 */

#include "cli.h"
#include "app_context.h"
#include "config.h"
#include "config/constants.h"
#include "log.h"
#include "platform.h"
#include "utilities.h"

#include "argparse.h"
#include "module_registry.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Store original command-line arguments for improved error reporting.
static int g_original_argc = 0;
static const char **g_original_argv = NULL;

#define MAX_STATIC_OPTIONS 128
#define MAX_TOTAL_OPTIONS (MAX_STATIC_OPTIONS + PRESETS_MAX_COUNT)
#define OPTIONS_COUNT(x) (int)(sizeof(x) / sizeof((x)[0]))

// --- Helper: append a block of options into the flat options buffer ---
static bool append_argparse_options(struct argparse_option *dest, int *total,
                                    int capacity,
                                    const struct argparse_option *src,
                                    int count) {
  if (*total + count > capacity) {
    log_fatal("Internal error: Exceeded maximum number of CLI options.");
    return false;
  }
  memcpy(&dest[*total], src, (size_t)count * sizeof(struct argparse_option));
  *total += count;
  return true;
}

// --- Forward Declarations ---
static bool validate_and_process_args(AppContext *app, int non_opt_argc,
                                      const char **non_opt_argv);
static int version_cb(struct argparse *self,
                      const struct argparse_option *option);
static int build_cli_options(struct argparse_option *options_buffer,
                             int max_options, AppConfig *config,
                             MemoryArena *arena, const char *active_input_type,
                             const char *active_output_type);
static bool apply_preset_if_requested(AppConfig *config);

static int mark_cli_source_callback(struct argparse *self,
                                    const struct argparse_option *option) {
  (void)self;
  if (option && option->data) {
    *(ConfigValueSource *)option->data = CONFIG_SOURCE_USER;
  }
  return 0;
}

// --- Callback to catch users trying to use presets as flags ---
static int preset_flag_warning_cb(struct argparse *self,
                                  const struct argparse_option *option) {
  (void)self;
  log_error(
      "'--%s' is not a valid flag. To load the '%s' preset, use '--preset %s'.",
      option->long_name, option->long_name, option->long_name);
  exit(EXIT_FAILURE);
  return 0;
}

void cli_print_usage(const char *prog_name, AppConfig *config,
                     MemoryArena *arena) {
  (void)prog_name;
  struct argparse argparse;
  struct argparse_option all_options[MAX_TOTAL_OPTIONS];
  const char *const usages[] = {
      "iq_tool -i <in_type> [in_file] -o <out_type> [out_file] [options]",
      NULL,
  };

  build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena, NULL, NULL);
  argparse_init(&argparse, all_options, usages, 0);
  argparse_describe(&argparse, NULL, NULL);
  argparse_usage(&argparse);
}

static int version_cb(struct argparse *self,
                      const struct argparse_option *option) {
  (void)self;
  (void)option;
#ifdef GIT_HASH
  fprintf(stdout, "%s revision %s\n", APP_NAME, GIT_HASH);
#else
  fprintf(stdout, "%s revision unknown\n", APP_NAME);
#endif
  exit(EXIT_SUCCESS);
}

static int build_cli_options(struct argparse_option *options_buffer,
                             int max_options, AppConfig *config,
                             MemoryArena *arena, const char *active_input_type,
                             const char *active_output_type) {
  int total_options = 0;
  struct argparse_option generic_options[] = {
      OPT_GROUP("Required Input & Output"),
      OPT_STRING('i', "input", &config->input.type_name,
                 "Specifies the input module.", NULL, 0, 0),
      OPT_STRING('o', "output", &config->output.module_name,
                 "Specifies the output module and optional file path", NULL, 0,
                 0),

      OPT_GROUP("Audio Output Options"),
      OPT_STRING(0, "audio-writer", &config->audio.path_arg,
                 "Save demodulated audio to a WAV file.", NULL, 0, 0),
      OPT_BOOLEAN(0, "audio-writer-rf64", &config->audio.writer_rf64,
                  "Use RF64 format for the audio writer (supports >4GB files).",
                  NULL, 0, 0),
      OPT_BOOLEAN(0, "mute-audio", &config->audio.mute,
                  "Disable speaker playback", NULL, 0, 0),

      OPT_GROUP("Input Processing Options"),
      OPT_FLOAT(0, "input-gain-multiplier", &config->dsp.input_gain,
                "Apply a linear gain multiplier to INPUT samples (before "
                "processing).",
                NULL, 0, 0),

      OPT_GROUP("Baseband Processing Options"),
      OPT_DOUBLE(
          0, "baseband-sample-rate", &config->baseband_sample_rate.rate_hz,
          "Baseband sample rate feeding into a demodulator in Hz.",
          mark_cli_source_callback,
          (intptr_t)&config->baseband_sample_rate.source, 0),
      OPT_STRING(
          0, "baseband-sample-format",
          &config->baseband_sample_format.format_str,
          "Baseband sample format feeding into a demodulator {cf32|cs16|...}.",
          mark_cli_source_callback,
          (intptr_t)&config->baseband_sample_format.source, 0),
      OPT_FLOAT(0, "baseband-gain-multiplier", &config->dsp.baseband_gain,
                "Apply a linear gain multiplier to baseband samples before "
                "demodulation.",
                NULL, 0, 0),

      OPT_GROUP("I/Q Output Options"),
      OPT_DOUBLE(0, "output-sample-rate", &config->output_sample_rate.rate_hz,
                 "Output sample rate in Hz.", mark_cli_source_callback,
                 (intptr_t)&config->output_sample_rate.source, 0),
      OPT_STRING(0, "output-sample-format", &config->output.sample_format_str,
                 "Sample format for output data {cs8|cu8|cs16|...}.",
                 mark_cli_source_callback,
                 (intptr_t)&config->output.format_source, 0),
      OPT_FLOAT(
          0, "output-gain-multiplier", &config->dsp.output_gain,
          "Apply a linear gain multiplier to OUTPUT samples before saving.",
          NULL, 0, 0),

      OPT_GROUP("General Options"),
      OPT_BOOLEAN(
          0, "raw-passthrough", &config->dsp.raw_passthrough,
          "Bypass all processing. Copies raw input bytes directly to output.",
          NULL, 0, 0),
      OPT_STRING(0, "preset", &config->preset_name,
                 "Use a preset for a common target.", NULL, 0, 0),
  };

  struct argparse_option sdr_general_options[] = {
      OPT_GROUP("SDR General Options"),
      OPT_DOUBLE(0, "sdr-rf-freq", &config->rf_freq_hz,
                 "(Required for SDR) Tuner center frequency in Hz", NULL, 0, 0),
      OPT_DOUBLE(
          0, "sdr-freq-offset", &config->sdr_general.frequency_offset_hz,
          "Frequency offset in Hz (e.g. 125e6 for HamItUp).", NULL, 0, 0),
      OPT_DOUBLE(0, "sdr-sample-rate", &config->input_sample_rate_hz,
                 "Set sample rate in Hz. (Device-specific default)", NULL, 0,
                 0),
      OPT_BOOLEAN(0, "sdr-bias-t", &config->sdr_general.bias_t_enable,
                  "(Optional) Enable Bias-T power.", NULL, 0, 0),
  };

  struct argparse_option final_options[] = {
      OPT_GROUP("Help & Version"),
      OPT_BOOLEAN('v', "version", NULL,
                  "show program's version number and exit", version_cb, 0,
                  OPT_NONEG),
      OPT_BOOLEAN('r', "revision", NULL,
                  "show program's revision number and exit", version_cb, 0,
                  OPT_NONEG),
      OPT_BOOLEAN('h', "help", NULL, "show this help message and exit",
                  argparse_help_cb, 0, OPT_NONEG),
      OPT_END(),
  };

  if (!append_argparse_options(options_buffer, &total_options, max_options,
                               generic_options, OPTIONS_COUNT(generic_options)))
    return -1;
  if (!append_argparse_options(options_buffer, &total_options, max_options,
                               sdr_general_options,
                               OPTIONS_COUNT(sdr_general_options)))
    return -1;
  module_populate_cli_options(options_buffer, &total_options, max_options,
                              active_input_type, active_output_type, arena);
  if (config->num_presets > 0) {
    struct argparse_option preset_header[] = {OPT_GROUP("Available Presets")};
    if (!append_argparse_options(options_buffer, &total_options, max_options,
                                 preset_header, OPTIONS_COUNT(preset_header)))
      return -1;
    struct argparse_option preset_options[PRESETS_MAX_COUNT];
    int presets_to_add = (config->num_presets > PRESETS_MAX_COUNT)
                             ? PRESETS_MAX_COUNT
                             : config->num_presets;
    for (int i = 0; i < presets_to_add; i++) {
      preset_options[i] =
          (struct argparse_option){.type = ARGPARSE_OPT_BOOLEAN,
                                   .long_name = config->presets[i].name,
                                   .help = config->presets[i].description,
                                   .flags = OPT_LONG_NOPREFIX,
                                   .callback = preset_flag_warning_cb,
                                   .value = NULL};
    }
    if (!append_argparse_options(options_buffer, &total_options, max_options,
                                 preset_options, presets_to_add))
      return -1;
  }
  if (!append_argparse_options(options_buffer, &total_options, max_options,
                               final_options, OPTIONS_COUNT(final_options)))
    return -1;
  return total_options;
}

bool cli_parse(int argc, char *argv[], AppContext *app) {
  AppConfig *config = (AppConfig *)app->config;
  MemoryArena *arena = &app->process_chain.setup_arena;
  g_original_argc = argc;
  g_original_argv = (const char **)argv;

  // --- Stage 1: Targeted Pre-scan & Module Defaults ---
  const char *pre_input = NULL;
  const char *pre_output = NULL;
  for (int arg_index = 1; arg_index < argc; arg_index++) {
    if ((strcmp(argv[arg_index], "-i") == 0 ||
         strcmp(argv[arg_index], "--input") == 0) &&
        arg_index + 1 < argc) {
      pre_input = argv[arg_index + 1];
    } else if (strncmp(argv[arg_index], "--input=", 8) == 0) {
      pre_input = argv[arg_index] + 8;
    } else if ((strcmp(argv[arg_index], "-o") == 0 ||
                strcmp(argv[arg_index], "--output") == 0) &&
               arg_index + 1 < argc) {
      pre_output = argv[arg_index + 1];
    } else if (strncmp(argv[arg_index], "--output=", 9) == 0) {
      pre_output = argv[arg_index] + 9;
    } else if (strcmp(argv[arg_index], "--preset") == 0 &&
               arg_index + 1 < argc) {
      config->preset_name = argv[arg_index + 1];
    } else if (strncmp(argv[arg_index], "--preset=", 9) == 0) {
      config->preset_name = argv[arg_index] + 9;
    }
  }
  if (pre_input) {
    const Module *module = module_get(pre_input, MODULE_TYPE_INPUT);
    if (module) {
      if (module->set_default_config)
        module->set_default_config(config);
    } else {
      pre_input = NULL; // Prevent spurious warnings if module doesn't exist
    }
  }
  if (pre_output) {
    const Module *module = module_get(pre_output, MODULE_TYPE_OUTPUT);
    if (module) {
      if (module->set_default_config)
        module->set_default_config(config);
    } else {
      pre_output = NULL; // Prevent spurious warnings if module doesn't exist
    }
  }

  // --- Stage 2: Apply Preset Baseline (Before User CLI Overrides) ---
  bool preset_matched = apply_preset_if_requested(config);

  // --- Stage 3: Parse User CLI Options (Overrides Preset & Defaults) ---
  struct argparse_option all_options[MAX_TOTAL_OPTIONS];
  if (build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena,
                        pre_input, pre_output) < 0) {
    return false;
  }

  struct argparse argparse;
  const char *const usages[] = {
      "iq_tool -i <in_type> [in_file] -o <out_type> [out_file] [options]",
      NULL,
  };
  argparse_init(&argparse, all_options, usages, 0);
  argparse_describe(&argparse, NULL, NULL);
  int non_opt_argc = argparse_parse(&argparse, argc, (const char **)argv);

  if (!preset_matched) {
    log_error(
        "Unknown preset '%s'. Check '%s' or --help for available presets.",
        config->preset_name, PRESETS_FILENAME);
    return false;
  }

  // --- Stage 4: Validate & Finalize Configuration ---
  if (!validate_and_process_args(app, non_opt_argc, argparse.out))
    return false;

  return true;
}

static bool apply_preset_if_requested(AppConfig *config) {
  if (!config->preset_name)
    return true;

  for (int preset_index = 0; preset_index < config->num_presets;
       preset_index++) {
    if (strcasecmp(config->preset_name, config->presets[preset_index].name) ==
        0) {
      const PresetDefinition *preset = &config->presets[preset_index];
      if (preset->rate_hz > 0.0) {
        config->output_sample_rate.rate_hz = preset->rate_hz;
        config->output_sample_rate.source = CONFIG_SOURCE_PRESET;
      }
      if (preset->baseband_sample_rate_hz > 0.0) {
        config->baseband_sample_rate.rate_hz = preset->baseband_sample_rate_hz;
        config->baseband_sample_rate.source = CONFIG_SOURCE_PRESET;
      }
      if (preset->output_sample_format) {
        config->output.sample_format_str = preset->output_sample_format;
        config->output.format_source = CONFIG_SOURCE_PRESET;
      }
      if (preset->baseband_sample_format) {
        config->baseband_sample_format.format_str =
            preset->baseband_sample_format;
        config->baseband_sample_format.source = CONFIG_SOURCE_PRESET;
      }
      if (preset->input_gain > 0.0f)
        config->dsp.input_gain = preset->input_gain;
      if (preset->output_gain > 0.0f)
        config->dsp.output_gain = preset->output_gain;
      if (preset->baseband_gain > 0.0f)
        config->dsp.baseband_gain = preset->baseband_gain;
      if (preset->dc_block_enable)
        config->dsp.dc_block.enable = true;
      if (preset->iq_correction_enable)
        config->dsp.iq_correction.enable = true;
      if (preset->output_agc_enable)
        config->dsp.output_agc.enable = true;
      if (preset->output_agc_target > 0.0f)
        config->dsp.output_agc.target_level = preset->output_agc_target;
      if (preset->baseband_agc_enable)
        config->dsp.baseband_agc.enable = true;
      if (preset->baseband_agc_target > 0.0f)
        config->dsp.baseband_agc.target_level = preset->baseband_agc_target;
      if (preset->frequency_shift_hz != 0.0)
        config->dsp.frequency_shift_hz = preset->frequency_shift_hz;
      if (preset->shift_after_resample)
        config->dsp.shift_after_resample = true;
      for (int stage_index = 0; stage_index < FILTER_MAX_CHAIN; stage_index++) {
        if (preset->lowpass_cutoff_hz[stage_index] > 0.0f)
          config->dsp.filter.args.lowpass[stage_index] =
              preset->lowpass_cutoff_hz[stage_index];
        if (preset->highpass_cutoff_hz[stage_index] > 0.0f)
          config->dsp.filter.args.highpass[stage_index] =
              preset->highpass_cutoff_hz[stage_index];
        if (preset->pass_range_str[stage_index])
          config->dsp.filter.args.pass_range[stage_index] =
              preset->pass_range_str[stage_index];
        if (preset->stopband_str[stage_index])
          config->dsp.filter.args.stopband[stage_index] =
              preset->stopband_str[stage_index];
      }
      if (preset->transition_width_hz > 0.0f)
        config->dsp.filter.args.transition_width = preset->transition_width_hz;
      if (preset->filter_taps > 0)
        config->dsp.filter.args.taps = preset->filter_taps;
      if (preset->default_filter_attenuation_db > 0.0f)
        config->dsp.filter.args.attenuation =
            preset->default_filter_attenuation_db;
      if (preset->filter_type_str)
        config->dsp.filter.args.type_str = preset->filter_type_str;
      if (preset->filter_fft_size > 0)
        config->dsp.filter.args.fft_size = preset->filter_fft_size;
      return true;
    }
  }
  return false;
}

static bool resolve_file_paths(AppConfig *config, MemoryArena *arena) {
  if (!config || !arena)
    return false;

  if (config->input.path_arg) {
    config->input.resolved_path =
        platform_resolve_input_path(config->input.path_arg, arena);
    if (!config->input.resolved_path)
      return false;
  }
  if (config->output.path_arg) {
    config->output.resolved_path =
        platform_resolve_output_path(config->output.path_arg, arena);
    if (!config->output.resolved_path)
      return false;
  }
  if (config->audio.path_arg) {
    config->audio.resolved_path =
        platform_resolve_output_path(config->audio.path_arg, arena);
    if (!config->audio.resolved_path)
      return false;
  }
  return true;
}

static bool validate_and_process_args(AppContext *app, int non_opt_argc,
                                      const char **non_opt_argv) {
  AppConfig *config = (AppConfig *)app->config;
  MemoryArena *arena = &app->process_chain.setup_arena;
  if (!config->input.type_name) {
    log_error("Missing required argument: --input <type>");
    return false;
  }
  const Module *selected_input_module =
      module_get(config->input.type_name, MODULE_TYPE_INPUT);
  if (!selected_input_module) {
    log_error("Invalid input type '%s'.", config->input.type_name);
    return false;
  }
  InputModuleInterface *selected_module_api =
      (InputModuleInterface *)selected_input_module->api;
  if (!selected_module_api)
    return false;

  if (selected_input_module->requires_input_path) {
    if (non_opt_argc < 1) {
      log_error("Missing <in_file> argument for input type '%s'.",
                config->input.type_name);
      return false;
    }
    config->input.path_arg = (char *)non_opt_argv[0];
    non_opt_argv++;
    non_opt_argc--;
  }

  if (!config->output.module_name) {
    log_error("Missing required argument: --output <type> [path]");
    return false;
  }
  const Module *selected_output_module =
      module_get(config->output.module_name, MODULE_TYPE_OUTPUT);
  if (!selected_output_module) {
    log_error("Invalid value for --output: '%s'.", config->output.module_name);
    return false;
  }
  if (selected_output_module->requires_output_path) {
    if (non_opt_argc < 1) {
      log_error("Missing <out_file> argument for '--output %s'.",
                config->output.module_name);
      return false;
    }
    config->output.path_arg = (char *)non_opt_argv[0];
    non_opt_argv++;
    non_opt_argc--;
  } else {
    config->output.path_arg = NULL;
  }

  if (non_opt_argc > 0) {
    log_error("Unexpected argument: '%s'", non_opt_argv[0]);
    return false;
  }

  if (selected_input_module->process_chain_mode ==
      PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL) {
    if (config->rf_freq_hz != 0.0 || config->input_sample_rate_hz != 0.0 ||
        config->sdr_general.frequency_offset_hz != 0.0 ||
        config->sdr_general.bias_t_enable) {
      log_error("SDR options (--sdr-rf-freq, --sdr-sample-rate, "
                "--sdr-freq-offset, --sdr-bias-t) cannot be used with '%s' "
                "input.",
                config->input.type_name);
      return false;
    }
  }

  if (!resolve_file_paths(config, arena))
    return false;

  // --- Validate Audio Writer Early ---
  if (config->audio.resolved_path && config->audio.resolved_path[0] != '\0') {
    if (!utility_verify_output_path(config, config->audio.resolved_path))
      return false;
  }

  // --- Final Validation Cascade ---
  if (!validate_output_type_and_sample_format(config))
    return false;

  // Validate options for ALL DSP modules (they are all always initialized)
  int num_modules;
  const Module *modules = module_get_all(&num_modules);
  for (int i = 0; i < num_modules; ++i) {
    if (modules[i].type == MODULE_TYPE_DSP) {
      const DspModuleInterface *dsp_mod =
          (const DspModuleInterface *)modules[i].api;
      if (dsp_mod && dsp_mod->validate_options) {
        if (!dsp_mod->validate_options(app)) {
          return false;
        }
      }
    }
  }

  if (selected_module_api->validate_options &&
      !selected_module_api->validate_options(app))
    return false;
  if (selected_module_api->validate_generic_options &&
      !selected_module_api->validate_generic_options(config))
    return false;

  const Module *out_mod_val =
      module_get(config->output.module_name, MODULE_TYPE_OUTPUT);
  if (out_mod_val) {
    OutputModuleInterface *out_api = (OutputModuleInterface *)out_mod_val->api;
    if (out_api && out_api->validate_options) {
      if (!out_api->validate_options(app))
        return false;
    }
  }

  if (!validate_option_combinations(config))
    return false;

  return true;
}
