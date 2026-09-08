/**
 * @file config.c
 */

#include "config.h"
#include "app_context.h"
#include "config/constants.h"
#include "log.h"
#include "module_registry.h"
#include "sample_format_table.h"
#include "utilities.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

bool validate_output_type_and_sample_format(AppConfig *config) {
  const Module *out_mod =
      module_get(config->output.module_name, MODULE_TYPE_OUTPUT, NULL);
  if (out_mod) {
    config->output.payload = out_mod->payload;
  }

  if (config->output.payload == PAYLOAD_AUDIO) {
    if (!config->baseband_sample_format.format_str) {
      if (out_mod && out_mod->module_defines_format) {
        config->baseband_sample_format.format_str = "cf32";
      } else {
        log_error("Missing required argument: --baseband-sample-format for "
                  "output '%s'.",
                  config->output.module_name);
        return false;
      }
    }
    config->baseband_sample_format.format =
        get_format_info_by_name(config->baseband_sample_format.format_str)
            ? get_format_info_by_name(config->baseband_sample_format.format_str)
                  ->format_enum
            : FORMAT_UNKNOWN;
    if (config->baseband_sample_format.format == FORMAT_UNKNOWN) {
      log_error("Invalid sample format '%s'.",
                config->baseband_sample_format.format_str);
      return false;
    }
  } else {
    if (config->output.sample_format_str) {
      config->output.format_provided = true;
    }
    if (!config->output.sample_format_str) {
      if (config->output.path_arg) {
        config->output.sample_format_str = "cs16";
        log_info("No output sample format specified; defaulting to 'cs16' for "
                 "file output.");
      } else if (out_mod && out_mod->module_defines_format) {
        config->output.sample_format_str = "cs16";
      } else {
        log_error("Missing required argument: --output-sample-format for "
                  "output '%s'.",
                  config->output.module_name);
        return false;
      }
    }
    config->output.sample_format =
        get_format_info_by_name(config->output.sample_format_str)
            ? get_format_info_by_name(config->output.sample_format_str)
                  ->format_enum
            : FORMAT_UNKNOWN;
    if (config->output.sample_format == FORMAT_UNKNOWN) {
      log_error("Invalid sample format '%s'.",
                config->output.sample_format_str);
      return false;
    }
  }
  return true;
}

bool validate_option_combinations(AppConfig *config) {

  if (config->audio.path_arg != NULL || config->audio.mute) {
    if (config->output.payload != PAYLOAD_AUDIO) {
      log_error("Options --audio-writer and --mute-audio can only be used with "
                "audio demodulation modules.");
      return false;
    }
  }

  if (config->dsp.output_agc.enable) {
    if (config->dsp.output_agc.target_level_arg != 0.0f) {
      if (config->dsp.output_agc.target_level_arg <= 0.0f ||
          config->dsp.output_agc.target_level_arg > 1.0f) {
        log_error("Invalid AGC target level %.2f. Must be between 0.0 and 1.0.",
                  config->dsp.output_agc.target_level_arg);
        return false;
      }
      config->dsp.output_agc.target_level =
          config->dsp.output_agc.target_level_arg;
    }
    if (config->dsp.output_gain != 1.0f) {
      log_error("Conflicting options: --output-agc and "
                "--output-gain-multiplier cannot be used together.");
      return false;
    }
  }
  if (config->dsp.baseband_agc.enable) {
    if (config->dsp.baseband_agc.target_level_arg != 0.0f) {
      if (config->dsp.baseband_agc.target_level_arg <= 0.0f ||
          config->dsp.baseband_agc.target_level_arg > 1.0f) {
        log_error("Invalid AGC target level %.2f. Must be between 0.0 and 1.0.",
                  config->dsp.baseband_agc.target_level_arg);
        return false;
      }
      config->dsp.baseband_agc.target_level =
          config->dsp.baseband_agc.target_level_arg;
    }
    if (config->dsp.baseband_gain != 1.0f) {
      log_error("Conflicting options: --baseband-agc and "
                "--baseband-gain-multiplier cannot be used together.");
      return false;
    }
  }

  if (config->output.payload == PAYLOAD_AUDIO) {
    if (config->output_sample_rate.provided) {
      log_error("--output-sample-rate cannot be used with demodulators. Please "
                "use --baseband-sample-rate.");
      return false;
    }
    if (config->output.type_provided) {
      log_error("--output-sample-format cannot be used with demodulators. "
                "Please use --baseband-sample-format.");
      return false;
    }
    if (config->dsp.output_gain != 1.0f) {
      log_error("--output-gain-multiplier cannot be used with demodulators. "
                "Please use --baseband-gain-multiplier.");
      return false;
    }
    if (config->dsp.output_agc.enable) {
      log_error("--output-agc cannot be used with demodulators. Please use "
                "--baseband-agc.");
      return false;
    }
  } else {
    if (config->baseband_sample_rate.provided) {
      log_error("--baseband-sample-rate cannot be used with raw outputs. "
                "Please use --output-sample-rate.");
      return false;
    }
    if (config->baseband_sample_format.provided) {
      log_error("--baseband-sample-format cannot be used with raw outputs. "
                "Please use --output-sample-format.");
      return false;
    }
    if (config->dsp.baseband_gain != 1.0f) {
      log_error("--baseband-gain-multiplier cannot be used with raw outputs. "
                "Please use --output-gain-multiplier.");
      return false;
    }
    if (config->dsp.baseband_agc.enable) {
      log_error("--baseband-agc cannot be used with raw outputs. Please use "
                "--output-agc.");
      return false;
    }
  }

  if ((config->output_sample_rate.provided ||
       config->baseband_sample_rate.provided) &&
      config->preset_name) {
    log_error("Options --output-sample-rate or --baseband-sample-rate cannot "
              "be used with --preset.");
    return false;
  }
  if (config->dsp.raw_passthrough) {
    if (config->dsp.filter.count > 0 ||
        config->dsp.frequency_shift_hz != 0.0f ||
        config->dsp.iq_correction.enable || config->dsp.dc_block.enable ||
        config->dsp.output_agc.enable || config->dsp.output_gain != 1.0f) {
      log_error("--raw-passthrough cannot be used with any other DSP options.");
      return false;
    }
    if (config->output.payload == PAYLOAD_AUDIO) {
      log_error("Option --raw-passthrough cannot be used with audio "
                "demodulation modules.");
      return false;
    }
  }

  return true;
}
