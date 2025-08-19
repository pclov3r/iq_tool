// src/input_manager.c

#include "input_manager.h"
#include <string.h>
#include <stdlib.h> // Needed for NULL

// --- Include the headers for ALL concrete input source implementations ---
#include "input_wav.h"
#include "input_rawfile.h"
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

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

// The master list is now built at runtime to allow for function calls.
static InputModule* all_modules = NULL;
static int num_all_modules = 0;
static bool modules_initialized = false;

// This function will build the list on the first call.
static void initialize_modules_list(void) {
    if (modules_initialized) {
        return;
    }

    // Define a temporary array using designated initializers for clarity and maintainability.
    InputModule temp_modules[] = {
        {
            .name = "wav",
            .ops = get_wav_input_ops(),
            .is_sdr = false,
            .set_default_config = NULL, // WAV module has no defaults to set
            .get_cli_options = wav_get_cli_options
        },
        {
            .name = "raw-file",
            .ops = get_raw_file_input_ops(),
            .is_sdr = false,
            .set_default_config = NULL, // Raw File module has no defaults to set
            .get_cli_options = rawfile_get_cli_options
        },
    #if defined(WITH_RTLSDR)
        {
            .name = "rtlsdr",
            .ops = get_rtlsdr_input_ops(),
            .is_sdr = true,
            .set_default_config = rtlsdr_set_default_config,
            .get_cli_options = rtlsdr_get_cli_options
        },
    #endif
    #if defined(WITH_SDRPLAY)
        {
            .name = "sdrplay",
            .ops = get_sdrplay_input_ops(),
            .is_sdr = true,
            .set_default_config = sdrplay_set_default_config,
            .get_cli_options = sdrplay_get_cli_options
        },
    #endif
    #if defined(WITH_HACKRF)
        {
            .name = "hackrf",
            .ops = get_hackrf_input_ops(),
            .is_sdr = true,
            .set_default_config = hackrf_set_default_config,
            .get_cli_options = hackrf_get_cli_options
        },
    #endif
    #if defined(WITH_BLADERF)
        {
            .name = "bladerf",
            .ops = get_bladerf_input_ops(),
            .is_sdr = true,
            .set_default_config = bladerf_set_default_config,
            .get_cli_options = bladerf_get_cli_options
        },
    #endif
    };

    num_all_modules = sizeof(temp_modules) / sizeof(temp_modules[0]);
    
    // Allocate memory on the heap and copy the data
    all_modules = (InputModule*)malloc(sizeof(temp_modules));
    if (all_modules) {
        memcpy(all_modules, temp_modules, sizeof(temp_modules));
    } else {
        // Handle catastrophic memory allocation failure
        num_all_modules = 0;
    }

    modules_initialized = true;
}

/**
 * @brief Iterates through all registered modules and applies their default settings.
 */
void input_manager_apply_defaults(AppConfig* config) {
    initialize_modules_list(); // Ensure the list is ready
    if (!all_modules) return;

    for (int i = 0; i < num_all_modules; ++i) {
        if (all_modules[i].set_default_config) {
            all_modules[i].set_default_config(config);
        }
    }
}

InputSourceOps* get_input_ops_by_name(const char* name) {
    initialize_modules_list(); // Ensure the list is ready
    if (!name || !all_modules) {
        return NULL;
    }
    for (int i = 0; i < num_all_modules; ++i) {
        if (strcasecmp(name, all_modules[i].name) == 0) {
            return all_modules[i].ops;
        }
    }
    return NULL; // Name not recognized
}

const InputModule* get_all_input_modules(int* count) {
    initialize_modules_list(); // Ensure the list is ready
    *count = num_all_modules;
    return all_modules;
}

bool is_sdr_input(const char* name) {
    if (!name) return false;
    initialize_modules_list(); // Ensure the list is ready

    for (int i = 0; i < num_all_modules; ++i) {
        if (strcasecmp(name, all_modules[i].name) == 0) {
            return all_modules[i].is_sdr;
        }
    }

    return false; // Name not found
}
