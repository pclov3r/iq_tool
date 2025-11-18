#include "module_manager.h"
#include "app_context.h"
#include "memory_arena.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

// --- Include the headers for ALL concrete input source implementations ---
#include "input_wav.h"
#include "input_rawfile.h"
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
#if defined(WITH_BLADERF)
#include "input_bladerf.h"
#endif

// --- Include the headers for ALL concrete output source implementations ---
#include "output_raw_file.h"
#include "output_wav.h"
#include "output_wav_rf64.h"
#include "output_stdout.h"


#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

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
            .api = get_wav_input_module_api(),
            .is_sdr = false,
            .set_default_config = NULL,
            .get_cli_options = wav_get_cli_options,
            .requires_output_path = false,
        },
        {
            .name = "raw-file",
            .type = MODULE_TYPE_INPUT,
            .api = get_raw_file_input_module_api(),
            .is_sdr = false,
            .set_default_config = NULL,
            .get_cli_options = rawfile_get_cli_options,
            .requires_output_path = false,
        },
    #if defined(WITH_RTLSDR)
        {
            .name = "rtlsdr",
            .type = MODULE_TYPE_INPUT,
            .api = get_rtlsdr_input_module_api(),
            .is_sdr = true,
            .set_default_config = rtlsdr_set_default_config,
            .get_cli_options = rtlsdr_get_cli_options,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_SDRPLAY)
        {
            .name = "sdrplay",
            .type = MODULE_TYPE_INPUT,
            .api = get_sdrplay_input_module_api(),
            .is_sdr = true,
            .set_default_config = sdrplay_set_default_config,
            .get_cli_options = sdrplay_get_cli_options,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_HACKRF)
        {
            .name = "hackrf",
            .type = MODULE_TYPE_INPUT,
            .api = get_hackrf_input_module_api(),
            .is_sdr = true,
            .set_default_config = hackrf_set_default_config,
            .get_cli_options = hackrf_get_cli_options,
            .requires_output_path = false,
        },
    #endif
    #if defined(WITH_BLADERF)
        {
            .name = "bladerf",
            .type = MODULE_TYPE_INPUT,
            .api = get_bladerf_input_module_api(),
            .is_sdr = true,
            .set_default_config = bladerf_set_default_config,
            .get_cli_options = bladerf_get_cli_options,
            .requires_output_path = false,
        },
    #endif
        {
            .name = "spyserver-client",
            .type = MODULE_TYPE_INPUT,
            .api = get_spyserver_client_input_module_api(),
            .is_sdr = true,
            .set_default_config = spyserver_client_set_default_config,
            .get_cli_options = spyserver_client_get_cli_options,
            .requires_output_path = false,
        },
        // --- OUTPUT MODULES ---
        {
            .name = "raw-file",
            .type = MODULE_TYPE_OUTPUT,
            .api = get_raw_file_output_module_api(),
            .is_sdr = false,
            .set_default_config = NULL,
            .get_cli_options = NULL,
            .requires_output_path = true,
        },
        {
            .name = "wav", // The command for the standard WAV format
            .type = MODULE_TYPE_OUTPUT,
            .api = get_wav_output_module_api(),
            .is_sdr = false,
            .set_default_config = NULL,
            .get_cli_options = NULL,
            .requires_output_path = true,
        },
        {
            .name = "wav-rf64", // The command for the modern RF64 format
            .type = MODULE_TYPE_OUTPUT,
            .api = get_wav_rf64_output_module_api(),
            .is_sdr = false,
            .set_default_config = NULL,
            .get_cli_options = NULL,
            .requires_output_path = true,
        },
        {
            .name = "stdout",
            .type = MODULE_TYPE_OUTPUT,
            .api = get_stdout_output_module_api(),
            .is_sdr = false,
            .set_default_config = NULL,
            .get_cli_options = NULL,
            .requires_output_path = false,
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
void module_manager_apply_defaults(AppConfig* config, MemoryArena* arena) {
    initialize_modules_list(arena); // Ensure the list is ready
    if (!all_modules) return;

    for (int i = 0; i < num_all_modules; ++i) {
        if (all_modules[i].set_default_config) {
            all_modules[i].set_default_config(config);
        }
    }
}

InputModuleInterface* module_manager_get_input_interface_by_name(const char* name, MemoryArena* arena) {
    const Module* found_module = _find_module_by_name_and_type(name, MODULE_TYPE_INPUT, arena);
    if (found_module) {
        return (InputModuleInterface*)found_module->api;
    }
    return NULL;
}

const Module* module_manager_get_output_module_by_name(const char* name, MemoryArena* arena) {
    return _find_module_by_name_and_type(name, MODULE_TYPE_OUTPUT, arena);
}

const Module* module_manager_get_all_modules(int* count, MemoryArena* arena) {
    initialize_modules_list(arena); // Ensure the list is ready
    *count = num_all_modules;
    return all_modules;
}

bool module_manager_is_sdr_module(const char* name, MemoryArena* arena) {
    const Module* mod = _find_module_by_name_and_type(name, MODULE_TYPE_INPUT, arena);
    return (mod != NULL && mod->is_sdr);
}

void module_manager_populate_cli_options(
    struct argparse_option* dest_buffer,
    int* total_opts_ptr,
    int max_opts,
    const char* active_input_type,
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

                if (active_input_type && all_modules[i].type == MODULE_TYPE_INPUT && strcasecmp(all_modules[i].name, active_input_type) != 0) {
                    for (int j = 0; j < count; j++) {
                        struct argparse_option* opt = &dest_buffer[*total_opts_ptr + j];
                        if (opt->type != ARGPARSE_OPT_GROUP) {
                            opt->value = NULL;
                        }
                    }
                }
                *total_opts_ptr += count;
            }
        }
    }
}
