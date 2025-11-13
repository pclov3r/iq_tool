/**
 * @file input_manager.h
 * @brief Defines the interface for managing all available input source modules.
 *
 * This module acts as a registry for all the different input sources that have
 * been compiled into the application (e.g., WAV, RTL-SDR, etc.). It provides
 * functions to look up a specific input module by name, apply default
 * configurations for all modules, and retrieve a list of all available modules
 * for generating help text.
 */

#ifndef MODULE_MANAGER_H_
#define MODULE_MANAGER_H_

#include "input_source.h" // Provides the core ModuleApi interface definition
#include "argparse.h"     // Provides the argparse_option struct for CLI options

// --- Forward Declarations ---
// We only use pointers to these structs, so we don't need their full definitions.
struct AppConfig;
struct MemoryArena;

// --- Type Definitions ---

/**
 * @struct InputModule
 * @brief A container for all the components that define a single input source module.
 */
typedef struct {
    const char* name; ///< The name used in the --input argument (e.g., "wav", "rtlsdr").
    ModuleApi* api; ///< Pointer to the core operational functions for this module.
    bool is_sdr; ///< Flag to indicate if this is an SDR source.
    void (*set_default_config)(struct AppConfig* config); ///< Pointer to the default config function.
    const struct argparse_option* (*get_cli_options)(int* count); ///< Pointer to the CLI option function.
} InputModule;


// --- Function Declarations ---

/**
 * @brief Gets the appropriate ModuleApi implementation based on a name.
 * @param name The name of the input source to find (e.g., "wav").
 * @param arena The memory arena, needed to initialize the module list on first call.
 * @return A pointer to the corresponding ModuleApi struct, or NULL if not found.
 */
ModuleApi* get_input_module_api_by_name(const char* name, struct MemoryArena* arena);

/**
 * @brief Gets a list of all registered and compiled-in input modules.
 * @param[out] count A pointer to an integer that will be filled with the number of modules.
 * @param arena The memory arena, needed to initialize the module list on first call.
 * @return A constant pointer to the array of InputModule structs.
 */
const InputModule* get_all_input_modules(int* count, struct MemoryArena* arena);

/**
 * @brief Iterates through all registered modules and applies their default settings.
 * @param config The application configuration struct to be modified.
 * @param arena The memory arena, needed to initialize the module list on first call.
 */
void module_manager_apply_defaults(struct AppConfig* config, struct MemoryArena* arena);

/**
 * @brief Checks if a given input type name corresponds to an SDR device.
 * @param name The name of the input source to check.
 * @param arena The memory arena, needed to initialize the module list on first call.
 * @return true if the name corresponds to an SDR, false otherwise.
 */
bool is_sdr_input(const char* name, struct MemoryArena* arena);

/**
 * @brief Populates a buffer with the CLI options from all registered input modules.
 *
 * This function iterates through all known modules and appends their argparse_option
 * structs to the destination buffer. It intelligently "disables" options for
 * inactive modules by setting their value pointers to NULL.
 *
 * @param dest_buffer The argparse_option array to be filled.
 * @param total_opts_ptr A pointer to the running count of options in the buffer.
 * @param max_opts The capacity of the destination buffer.
 * @param active_input_type The name of the currently active input module.
 * @param arena The memory arena, needed to initialize the module list.
 */
void module_manager_populate_cli_options(
    struct argparse_option* dest_buffer,
    int* total_opts_ptr,
    int max_opts,
    const char* active_input_type,
    struct MemoryArena* arena
);

#endif // MODULE_MANAGER_H_
