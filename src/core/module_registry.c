/**
 * @file module_registry.c
 */

#include "module_registry.h"
#include "app_context.h"
#include "log.h"
#include "mem_arena.h"
#include "module_defaults.h"
#include <stdlib.h>
#include <string.h>

// --- Module Declarations Macros ---
// These macros replace the need for dozens of individual header files.
// By defining them here, a developer only needs to edit THIS file to add a new
// module.
#define DECLARE_INPUT_MODULE(name)                                             \
  InputModuleInterface *input_##name##_get_module_api(void);                   \
  const struct argparse_option *input_##name##_get_cli_options(int *count);    \
  void input_##name##_set_default_config(struct AppConfig *config)

#define DECLARE_OUTPUT_MODULE(name)                                            \
  OutputModuleInterface *output_##name##_get_module_api(void);                 \
  const struct argparse_option *output_##name##_get_cli_options(int *count)

#define DECLARE_DSP_MODULE(name)                                               \
  const struct DspModuleInterface *dsp_##name##_get_api(void)

// --- Input Modules ---
DECLARE_INPUT_MODULE(wav);
DECLARE_INPUT_MODULE(rawfile);
DECLARE_INPUT_MODULE(stdin);
#if defined(WITH_RTLSDR)
DECLARE_INPUT_MODULE(rtlsdr);
#endif
#if defined(WITH_SDRPLAY)
DECLARE_INPUT_MODULE(sdrplay);
#endif
#if defined(WITH_HACKRF)
DECLARE_INPUT_MODULE(hackrf);
#endif
#if defined(WITH_AIRSPY)
DECLARE_INPUT_MODULE(airspy);
#endif
#if defined(WITH_AIRSPYHF)
DECLARE_INPUT_MODULE(airspyhf);
#endif
#if defined(WITH_HYDRASDR)
DECLARE_INPUT_MODULE(hydrasdr);
#endif
#if defined(WITH_BLADERF)
DECLARE_INPUT_MODULE(bladerf);
#endif
DECLARE_INPUT_MODULE(spyserver_client);

// --- Output Modules ---
DECLARE_OUTPUT_MODULE(rawfile);
DECLARE_OUTPUT_MODULE(wav);
DECLARE_OUTPUT_MODULE(wav_rf64);
DECLARE_OUTPUT_MODULE(stdout);
DECLARE_OUTPUT_MODULE(directpipe);
#if defined(WITH_NRSC5)
DECLARE_OUTPUT_MODULE(nrsc5);
#endif
DECLARE_OUTPUT_MODULE(wfm);
DECLARE_OUTPUT_MODULE(nfm);
DECLARE_OUTPUT_MODULE(noaawx);
DECLARE_OUTPUT_MODULE(am);

// --- DSP Modules ---
DECLARE_DSP_MODULE(filter);
DECLARE_DSP_MODULE(iq_correct);
DECLARE_DSP_MODULE(dcblock);
DECLARE_DSP_MODULE(freq_shift);
DECLARE_DSP_MODULE(agc);
DECLARE_DSP_MODULE(resampler);

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

// --- Private struct and forward declarations ---
typedef struct {
  const char *active_input;
  const char *active_output;
} ModuleWarningContext;

static int inactive_option_warning_cb(struct argparse *self,
                                      const struct argparse_option *opt);

// The master list is now built at runtime to allow for function calls.
static Module *all_modules = NULL;
static int num_all_modules = 0;
static bool modules_initialized = false;

// This function now accepts an arena to perform its allocation.
static void initialize_modules_list(MemoryArena *arena) {
  if (modules_initialized) {
    return;
  }

  // Define a temporary array using designated initializers for clarity and
  // maintainability.
  Module temp_modules[] = {
      // --- DSP MODULES ---
      // Internal DSP process_chain modules (placed first so their CLI options
      // appear
      // at the top)
      {
          .name = "dc_block",
          .type = MODULE_TYPE_DSP,
          .api = (void *)dsp_dcblock_get_api(),
      },
      {
          .name = "iq_correct",
          .type = MODULE_TYPE_DSP,
          .api = (void *)dsp_iq_correct_get_api(),
      },
      {
          .name = "freq_shift",
          .type = MODULE_TYPE_DSP,
          .api = (void *)dsp_freq_shift_get_api(),
      },
      {
          .name = "filter",
          .type = MODULE_TYPE_DSP,
          .api = (void *)dsp_filter_get_api(),
      },
      {
          .name = "resampler",
          .type = MODULE_TYPE_DSP,
          .api = (void *)dsp_resampler_get_api(),
      },
      {
          .name = "agc",
          .type = MODULE_TYPE_DSP,
          .api = (void *)dsp_agc_get_api(),
      },

      // --- INPUT MODULES ---
      {
          .name = "wav",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db = 0.0f,
          .api = input_wav_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL,
          .set_default_config = NULL,
          .get_cli_options = input_wav_get_cli_options,
          .requires_input_path = true,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = WAV_DEMOD_AUDIO_BUFFER_SIZE,
      },
      {
          .name = "rawfile",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db = 0.0f,
          .api = input_rawfile_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL,
          .set_default_config = NULL,
          .get_cli_options = input_rawfile_get_cli_options,
          .requires_input_path = true,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = RAWFILE_DEMOD_AUDIO_BUFFER_SIZE,
      },
      {
          .name = "stdin",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db = 0.0f,
          .api = input_stdin_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL,
          .set_default_config = NULL,
          .get_cli_options = input_stdin_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = STDIN_DEMOD_AUDIO_BUFFER_SIZE,
      },
#if defined(WITH_RTLSDR)
      {
          .name = "rtlsdr",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db = RTLSDR_DEFAULT_FILTER_ATTENUATION_DB,
          .api = input_rtlsdr_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_rtlsdr_set_default_config,
          .get_cli_options = input_rtlsdr_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = RTLSDR_DEMOD_AUDIO_BUFFER_SIZE,
      },
#endif
#if defined(WITH_SDRPLAY)
      {
          .name = "sdrplay",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db =
              SDRPLAY_DEFAULT_FILTER_ATTENUATION_DB,
          .api = input_sdrplay_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_sdrplay_set_default_config,
          .get_cli_options = input_sdrplay_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = SDRPLAY_DEMOD_AUDIO_BUFFER_SIZE,
      },
#endif
#if defined(WITH_HACKRF)
      {
          .name = "hackrf",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db = HACKRF_DEFAULT_FILTER_ATTENUATION_DB,
          .api = input_hackrf_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_hackrf_set_default_config,
          .get_cli_options = input_hackrf_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = HACKRF_DEMOD_AUDIO_BUFFER_SIZE,
      },
#endif
#if defined(WITH_AIRSPY)
      {
          .name = "airspy",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db = AIRSPY_DEFAULT_FILTER_ATTENUATION_DB,
          .api = input_airspy_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_airspy_set_default_config,
          .get_cli_options = input_airspy_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = AIRSPY_DEMOD_AUDIO_BUFFER_SIZE,
      },
#endif
#if defined(WITH_AIRSPYHF)
      {
          .name = "airspyhf",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db =
              AIRSPYHF_DEFAULT_FILTER_ATTENUATION_DB,
          .api = input_airspyhf_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_airspyhf_set_default_config,
          .get_cli_options = input_airspyhf_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = AIRSPYHF_DEMOD_AUDIO_BUFFER_SIZE,
      },
#endif
#if defined(WITH_HYDRASDR)
      {
          .name = "hydrasdr",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db =
              HYDRASDR_DEFAULT_FILTER_ATTENUATION_DB,
          .api = input_hydrasdr_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_hydrasdr_set_default_config,
          .get_cli_options = input_hydrasdr_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = HYDRASDR_DEMOD_AUDIO_BUFFER_SIZE,
      },
#endif
#if defined(WITH_BLADERF)
      {
          .name = "bladerf",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db =
              BLADERF_DEFAULT_FILTER_ATTENUATION_DB,
          .api = input_bladerf_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_bladerf_set_default_config,
          .get_cli_options = input_bladerf_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = BLADERF_DEMOD_AUDIO_BUFFER_SIZE,
      },
#endif
      {
          .name = "spyserver-client",
          .type = MODULE_TYPE_INPUT,
          .default_filter_attenuation_db = 0.0f,
          .api = input_spyserver_client_get_module_api(),
          .process_chain_mode = PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH,
          .set_default_config = input_spyserver_client_set_default_config,
          .get_cli_options = input_spyserver_client_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .default_demod_audio_buffer_size = SPYSERVER_DEMOD_AUDIO_BUFFER_SIZE,
      },

      // --- OUTPUT MODULES ---
      {
          .name = "rawfile",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_IQ,
          .api = output_rawfile_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_rawfile_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = true,
      },
      {
          .name = "wav", // The command for the standard WAV format
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_IQ,
          .api = output_wav_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_wav_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = true,
      },
      {
          .name = "wav-rf64", // The command for the modern RF64 format
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_IQ,
          .api = output_wav_rf64_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_wav_rf64_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = true,
      },
      {
          .name = "stdout",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_IQ,
          .api = output_stdout_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_stdout_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
      },
      {
          .name = "directpipe",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_IQ,
          .api = output_directpipe_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_directpipe_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
      },
#if defined(WITH_NRSC5)
      {
          .name = "nrsc5",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_AUDIO,
          .api = output_nrsc5_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_nrsc5_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .module_defines_format = true,
      },
#endif
      {
          .name = "wfm",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_AUDIO,
          .api = output_wfm_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_wfm_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .module_defines_format = true,
      },
      {
          .name = "nfm",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_AUDIO,
          .api = output_nfm_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_nfm_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .module_defines_format = true,
      },
      {
          .name = "noaawx",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_AUDIO,
          .api = output_noaawx_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_noaawx_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .module_defines_format = true,
      },
      {
          .name = "am",
          .type = MODULE_TYPE_OUTPUT,
          .payload = PAYLOAD_AUDIO,
          .api = output_am_get_module_api(),
          .set_default_config = NULL,
          .get_cli_options = output_am_get_cli_options,
          .requires_input_path = false,
          .requires_output_path = false,
          .module_defines_format = true,
      },

  };

  num_all_modules = sizeof(temp_modules) / sizeof(temp_modules[0]);

  all_modules = (Module *)mem_arena_alloc(arena, sizeof(temp_modules), true);
  if (all_modules) {
    memcpy(all_modules, temp_modules, sizeof(temp_modules));
  } else {
    num_all_modules = 0;
  }

  modules_initialized = true;
}

static const Module *_find_module_by_name_and_type(const char *name,
                                                   ModuleType type,
                                                   MemoryArena *arena) {
  initialize_modules_list(arena); // Ensure the list is ready
  if (!name || !all_modules) {
    return NULL;
  }
  for (int i = 0; i < num_all_modules; ++i) {
    if (all_modules[i].type == type &&
        strcasecmp(name, all_modules[i].name) == 0) {
      return &all_modules[i];
    }
  }
  return NULL; // Not found
}

/**
 * @brief Iterates through all registered modules and applies their default
 * settings.
 */
void module_apply_defaults(AppConfig *config, MemoryArena *arena) {
  initialize_modules_list(arena); // Ensure the list is ready
  if (!all_modules)
    return;

  for (int i = 0; i < num_all_modules; ++i) {
    if (all_modules[i].set_default_config) {
      all_modules[i].set_default_config(config);
    }
  }
}

const Module *module_get_all(int *count, MemoryArena *arena) {
  initialize_modules_list(arena); // Ensure the list is ready
  *count = num_all_modules;
  return all_modules;
}

bool module_is_live_source(const char *name, MemoryArena *arena) {
  const Module *mod =
      _find_module_by_name_and_type(name, MODULE_TYPE_INPUT, arena);
  return (mod != NULL &&
          mod->process_chain_mode == PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH);
}

void module_populate_cli_options(struct argparse_option *dest_buffer,
                                 int *total_opts_ptr, int max_opts,
                                 const char *active_input_type,
                                 const char *active_output_type,
                                 struct MemoryArena *arena) {
  initialize_modules_list(arena);
  if (!all_modules)
    return;

  for (int i = 0; i < num_all_modules; ++i) {
    const struct argparse_option *(*get_opts_fn)(int *) =
        all_modules[i].get_cli_options;
    if (!get_opts_fn && all_modules[i].type == MODULE_TYPE_DSP) {
      const DspModuleInterface *dsp =
          (const DspModuleInterface *)all_modules[i].api;
      if (dsp)
        get_opts_fn = dsp->get_cli_options;
    }

    if (get_opts_fn) {
      int count = 0;
      const struct argparse_option *opts = get_opts_fn(&count);
      if (opts && count > 0) {
        if (*total_opts_ptr + count > max_opts) {
          log_fatal("Internal error: Exceeded maximum number of CLI options.");
          return;
        }

        memcpy(&dest_buffer[*total_opts_ptr], opts,
               count * sizeof(struct argparse_option));

        // Allocate our warning context once
        ModuleWarningContext *warning_context =
            (ModuleWarningContext *)mem_arena_alloc(
                arena, sizeof(ModuleWarningContext), true);
        if (warning_context) {
          warning_context->active_input = active_input_type;
          warning_context->active_output = active_output_type;
        }

        bool is_inactive_input =
            (active_input_type && all_modules[i].type == MODULE_TYPE_INPUT &&
             strcasecmp(all_modules[i].name, active_input_type) != 0);
        bool is_inactive_output =
            (active_output_type && all_modules[i].type == MODULE_TYPE_OUTPUT &&
             strcasecmp(all_modules[i].name, active_output_type) != 0);
        // DSP modules are always active in the parser because their flags ARE
        // how you activate them.

        if (is_inactive_input || is_inactive_output) {
          for (int j = 0; j < count; j++) {
            struct argparse_option *opt = &dest_buffer[*total_opts_ptr + j];
            if (opt->type != ARGPARSE_OPT_GROUP) {
              opt->callback = inactive_option_warning_cb;
              opt->data = (intptr_t)warning_context;
            }
          }
        }
        *total_opts_ptr += count;
      }
    }
  }
}

const Module *module_get(const char *name, ModuleType type,
                         MemoryArena *arena) {
  return _find_module_by_name_and_type(name, type, arena);
}

const struct DspModuleInterface *get_dsp_module(const char *name,
                                                struct MemoryArena *arena) {
  const Module *mod = module_get(name, MODULE_TYPE_DSP, arena);
  if (mod)
    return (const struct DspModuleInterface *)mod->api;
  return NULL;
}

// --- Private Helper Functions ---

// Callback triggered when a user provides a flag for a module that isn't
// currently active.
static int inactive_option_warning_cb(struct argparse *self,
                                      const struct argparse_option *opt) {
  if (opt->type != ARGPARSE_OPT_GROUP && opt->data != 0) {
    ModuleWarningContext *context = (ModuleWarningContext *)opt->data;

    const char *user_value = self->optvalue;

    if (user_value) {
      log_warn("Ignoring '--%s %s' because it does not apply to the selected "
               "input ('%s') or output ('%s') module.",
               opt->long_name, user_value,
               context->active_input ? context->active_input : "unknown",
               context->active_output ? context->active_output : "unknown");
    } else {
      log_warn("Ignoring '--%s' because it does not apply to the selected "
               "input ('%s') or output ('%s') module.",
               opt->long_name,
               context->active_input ? context->active_input : "unknown",
               context->active_output ? context->active_output : "unknown");
    }
  }
  return 0;
}
