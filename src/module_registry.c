#include "module_registry.h"
#include "app_context.h"
#include "mem_arena.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

// --- Include the headers for ALL concrete input source implementations ---
#include "input_wav.h"
#include "input_rawfile.h"
#include "input_stdin.h"
#include "input_spyserver_client.h"
#if defined(WITH_RTLSDR)
#include "input_rtlsdr.h"
#endif
#if defined(WITH_SDRPLAY)
#include "input_sdrplay.h"
#endif
#if defined(WITH_HACKRF)
#include "input_hackrf.h"
#endif
#if defined(WITH_AIRSPY)
#include "input_airspy.h"
#endif
#if defined(WITH_AIRSPYHF)
#include "input_airspyhf.h"
#endif
#if defined(WITH_BLADERF)
#include "input_bladerf.h"
#endif

// --- Include the headers for ALL concrete output source implementations ---
#include "output_rawfile.h"
#include "output_wav.h"
#include "output_wav_rf64.h"
#include "output_stdout.h"
#include "output_directpipe.h"
#if defined(WITH_NRSC5)
#include "output_nrsc5.h"
#endif
#include "output_wfm.h"
#include "output_nfm.h"
#include "output_am.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

// --- Private struct and forward declarations ---
typedef struct {
    const char* active_input;
    const char* active_output;
} InactiveWarningContext;

static int inactive_option_warning_cb(struct argparse *self, const struct argparse_option *opt);

// The master list is now built at runtime to allow for function calls.
static Module* all_modules = NULL;
static int num_all_modules = 0;
static bool modules_initialized = false;

// This function now accepts an arena to perform its allocation.
static void initialize_modules_list(MemoryArena* arena) {
    if (modules_initialized) {
        return;
    }

    // Define a temporary array using designated initializers for clarity and maintainability.
    Module temp_modules[] = {
        // --- INPUT MODULES ---
        {
            .name = "wav",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = 0.0f,
            .api = input_wav_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_SYNCHRONOUS_PULL,
            .set_default_config = NULL,
            .get_cli_options = wav_input_get_cli_options,
            .requires_input_path = true,
            .requires_output_path = false,
        },
        {
            .name = "rawfile",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = 0.0f,
            .api = input_rawfile_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_SYNCHRONOUS_PULL,
            .set_default_config = NULL,
            .get_cli_options = rawfile_input_get_cli_options,
            .requires_input_path = true,
            .requires_output_path = false,
        },
        {
            .name = "stdin",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = 0.0f,
            .api = input_stdin_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_SYNCHRONOUS_PULL,
            .set_default_config = NULL,
            .get_cli_options = stdin_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
    #if defined(WITH_RTLSDR)
        {
            .name = "rtlsdr",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = RTLSDR_DEFAULT_FILTER_ATTENUATION_DB,
            .api = input_rtlsdr_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT,
            .set_default_config = rtlsdr_set_default_config,
            .get_cli_options = rtlsdr_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_SDRPLAY)
        {
            .name = "sdrplay",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = SDRPLAY_DEFAULT_FILTER_ATTENUATION_DB,
            .api = input_sdrplay_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT,
            .set_default_config = sdrplay_set_default_config,
            .get_cli_options = sdrplay_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_HACKRF)
        {
            .name = "hackrf",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = HACKRF_DEFAULT_FILTER_ATTENUATION_DB,
            .api = input_hackrf_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT,
            .set_default_config = hackrf_set_default_config,
            .get_cli_options = hackrf_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_AIRSPY)
        {
            .name = "airspy",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = AIRSPY_DEFAULT_FILTER_ATTENUATION_DB,
            .api = input_airspy_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT,
            .set_default_config = airspy_set_default_config,
            .get_cli_options = airspy_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_AIRSPYHF)
        {
            .name = "airspyhf",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = AIRSPYHF_DEFAULT_FILTER_ATTENUATION_DB,
            .api = input_airspyhf_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT,
            .set_default_config = airspyhf_set_default_config,
            .get_cli_options = airspyhf_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_BLADERF)
        {
            .name = "bladerf",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = BLADERF_DEFAULT_FILTER_ATTENUATION_DB,
            .api = input_bladerf_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT,
            .set_default_config = bladerf_set_default_config,
            .get_cli_options = bladerf_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
    #endif
        {
            .name = "spyserver-client",
            .type = MODULE_TYPE_INPUT,
            .default_filter_attenuation_db = 0.0f,
            .api = input_spyserver_client_get_module_api(),
            .pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT,
            .set_default_config = spyserver_client_set_default_config,
            .get_cli_options = spyserver_client_input_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
        // --- OUTPUT MODULES ---
        {
            .name = "rawfile",
            .type = MODULE_TYPE_OUTPUT,
            .payload = PAYLOAD_IQ,
            .api = output_rawfile_get_module_api(),
            .set_default_config = NULL,
            .get_cli_options = rawfile_output_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = true,
        },
        {
            .name = "wav", // The command for the standard WAV format
            .type = MODULE_TYPE_OUTPUT,
            .payload = PAYLOAD_IQ,
            .api = output_wav_get_module_api(),
            .set_default_config = NULL,
            .get_cli_options = wav_output_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = true,
        },
        {
            .name = "wav-rf64", // The command for the modern RF64 format
            .type = MODULE_TYPE_OUTPUT,
            .payload = PAYLOAD_IQ,
            .api = output_wav_rf64_get_module_api(),
            .set_default_config = NULL,
            .get_cli_options = wav_rf64_output_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = true,
        },
        {
            .name = "stdout",
            .type = MODULE_TYPE_OUTPUT,
            .payload = PAYLOAD_IQ,
            .api = output_stdout_get_module_api(),
            .set_default_config = NULL,
            .get_cli_options = stdout_output_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
        },
        {
            .name = "directpipe",
            .type = MODULE_TYPE_OUTPUT,
            .payload = PAYLOAD_IQ,
            .api = output_directpipe_get_module_api(),
            .set_default_config = NULL,
            .get_cli_options = directpipe_output_get_cli_options,
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
            .get_cli_options = nrsc5_output_get_cli_options,
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
            .get_cli_options = wfm_output_get_cli_options,
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
            .get_cli_options = nfm_output_get_cli_options,
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
            .get_cli_options = am_output_get_cli_options,
            .requires_input_path = false,
            .requires_output_path = false,
            .module_defines_format = true,
        },
    };

    num_all_modules = sizeof(temp_modules) / sizeof(temp_modules[0]);

    all_modules = (Module*)mem_arena_alloc(arena, sizeof(temp_modules), true);
    if (all_modules) {
        memcpy(all_modules, temp_modules, sizeof(temp_modules));
    } else {
        num_all_modules = 0;
    }

    modules_initialized = true;
}

static const Module* _find_module_by_name_and_type(const char* name, ModuleType type, MemoryArena* arena) {
    initialize_modules_list(arena); // Ensure the list is ready
    if (!name || !all_modules) {
        return NULL;
    }
    for (int i = 0; i < num_all_modules; ++i) {
        if (all_modules[i].type == type && strcasecmp(name, all_modules[i].name) == 0) {
            return &all_modules[i];
        }
    }
    return NULL; // Not found
}

/**
 * @brief Iterates through all registered modules and applies their default settings.
 */
void module_apply_defaults(AppConfig* config, MemoryArena* arena) {
    initialize_modules_list(arena); // Ensure the list is ready
    if (!all_modules) return;

    for (int i = 0; i < num_all_modules; ++i) {
        if (all_modules[i].set_default_config) {
            all_modules[i].set_default_config(config);
        }
    }
}

const Module* module_get_all(int* count, MemoryArena* arena) {
    initialize_modules_list(arena); // Ensure the list is ready
    *count = num_all_modules;
    return all_modules;
}

bool module_is_live_source(const char* name, MemoryArena* arena) {
    const Module* mod = _find_module_by_name_and_type(name, MODULE_TYPE_INPUT, arena);
    return (mod != NULL && mod->pipeline_mode == PIPELINE_MODE_BUFFERED_INPUT);
}

void module_populate_cli_options(
    struct argparse_option* dest_buffer,
    int* total_opts_ptr,
    int max_opts,
    const char* active_input_type,
    const char* active_output_type,
    struct MemoryArena* arena)
{
    initialize_modules_list(arena);
    if (!all_modules) return;

    for (int i = 0; i < num_all_modules; ++i) {
        if (all_modules[i].get_cli_options) {
            int count = 0;
            const struct argparse_option* opts = all_modules[i].get_cli_options(&count);
            if (opts && count > 0) {
                if (*total_opts_ptr + count > max_opts) {
                    log_fatal("Internal error: Exceeded maximum number of CLI options.");
                    return;
                }

                memcpy(&dest_buffer[*total_opts_ptr], opts, count * sizeof(struct argparse_option));


                // Allocate our warning context once
                InactiveWarningContext* warning_context = (InactiveWarningContext*)mem_arena_alloc(arena, sizeof(InactiveWarningContext), true);
                if (warning_context) {
                    warning_context->active_input = active_input_type;
                    warning_context->active_output = active_output_type;
                }

                // Check if this module is INACTIVE
                bool is_inactive_input = (active_input_type && all_modules[i].type == MODULE_TYPE_INPUT && strcasecmp(all_modules[i].name, active_input_type) != 0);
                bool is_inactive_output = (active_output_type && all_modules[i].type == MODULE_TYPE_OUTPUT && strcasecmp(all_modules[i].name, active_output_type) != 0);

                if (is_inactive_input || is_inactive_output) {
                    for (int j = 0; j < count; j++) {
                        struct argparse_option* opt = &dest_buffer[*total_opts_ptr + j];
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

const Module* module_get(const char* name, ModuleType type, MemoryArena* arena) {
    return _find_module_by_name_and_type(name, type, arena);
}

// --- Private Helper Functions ---

// Callback triggered when a user provides a flag for a module that isn't currently active.
static int inactive_option_warning_cb(struct argparse *self, const struct argparse_option *opt) {
    if (opt->type != ARGPARSE_OPT_GROUP && opt->data != 0) {
        InactiveWarningContext* context = (InactiveWarningContext*)opt->data;

        const char* user_value = self->optvalue;

        if (user_value) {
            log_warn("Ignoring '--%s %s' because it does not apply to the selected input ('%s') or output ('%s') module.",
                     opt->long_name, user_value,
                     context->active_input ? context->active_input : "unknown",
                     context->active_output ? context->active_output : "unknown");
        } else {
            log_warn("Ignoring '--%s' because it does not apply to the selected input ('%s') or output ('%s') module.",
                     opt->long_name,
                     context->active_input ? context->active_input : "unknown",
                     context->active_output ? context->active_output : "unknown");
        }
    }
    return 0;
}
