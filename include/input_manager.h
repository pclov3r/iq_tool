// input_manager.h

#ifndef INPUT_MANAGER_H_
#define INPUT_MANAGER_H_

#include "input_source.h"
#include "argparse.h"

/**
 * @brief A structure to hold all function pointers for a given input module.
 */
typedef struct {
    const char* name; // The name used in the --input argument (e.g., "wav", "rtlsdr")
    InputSourceOps* ops; // Pointer to the core operational functions
    bool is_sdr; // Flag to indicate if this is an SDR source
    void (*set_default_config)(AppConfig* config); // Pointer to the default config function
    const struct argparse_option* (*get_cli_options)(int* count); // Pointer to the CLI option function
} InputModule;


/**
 * @brief Gets the appropriate InputSourceOps implementation based on a name.
 * MODIFIED: Now requires a memory arena to initialize the module list on first call.
 */
InputSourceOps* get_input_ops_by_name(const char* name, MemoryArena* arena);

/**
 * @brief Gets a list of all registered and compiled-in input modules.
 * MODIFIED: Now requires a memory arena to initialize the module list on first call.
 */
const InputModule* get_all_input_modules(int* count, MemoryArena* arena);

/**
 * @brief Iterates through all registered modules and applies their default settings.
 * MODIFIED: Now requires a memory arena to initialize the module list on first call.
 */
void input_manager_apply_defaults(AppConfig* config, MemoryArena* arena);

/**
 * @brief Checks if a given input type name corresponds to an SDR device.
 * MODIFIED: Now requires a memory arena to initialize the module list on first call.
 */
bool is_sdr_input(const char* name, MemoryArena* arena);

#endif // INPUT_MANAGER_H_
