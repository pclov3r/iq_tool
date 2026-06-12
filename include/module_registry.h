/**
 * @file module_registry.h
 * @brief Defines the interface for managing all available application modules.
 *
 * This module acts as a registry for all the different pluggable modules that have
 * been compiled into the application (e.g., WAV, RTL-SDR, etc.). It provides
 * functions to look up a specific module by name, apply default
 * configurations, and retrieve the list of all available modules.
 */

#ifndef MODULE_REGISTRY_H_
#define MODULE_REGISTRY_H_

#include "module.h" // Provides the core InputModuleInterface interface definition
#include "common_types.h"

// --- Forward Declarations ---
// We only use pointers to these structs, so we don't need their full definitions.
struct AppConfig;
struct MemoryArena;

// --- Type Definitions ---

/**
 * @struct Module
 * @brief A container for all the components that define a single module.
 */
typedef struct Module {
    const char* name; ///< The name used in the --input argument (e.g., "wav", "rtlsdr").
    ModuleType type;
    void* api; ///< Generic pointer to the module's interface (e.g., InputModuleInterface* or OutputModuleInterface*).
    PipelineMode pipeline_mode; ///< Flag to indicate if this is a live source source.
    void (*set_default_config)(struct AppConfig* config); ///< Pointer to the default config function.
    const struct argparse_option* (*get_cli_options)(int* count); ///< Pointer to the CLI option function.
    bool requires_output_path; ///< For output modules, indicates if a file path argument is needed.
    bool requires_input_path;  // For INPUT modules (read from file)
    bool module_defines_format;
    OutputPayload payload;
    float default_filter_attenuation_db;
    size_t default_demod_audio_buffer_size; ///< Downstream demodulator audio buffer size dictated by this input source.
} Module;

// --- Function Declarations ---

/**
 * @brief Finds an INPUT module by name and returns its specific API.
 * @param name The name of the input source to find (e.g., "wav").
 * @param arena The memory arena, needed to initialize the module list on first call.
 * @return A pointer to the corresponding InputModuleInterface struct, or NULL if not found.
 */
const Module* module_get(const char* name, ModuleType type, struct MemoryArena* arena);

/**
 * @brief Finds a module by name and returns its full registration struct.
 * @param name The name of the module to find.
 * @param arena The memory arena, needed to initialize the module list on first call.
 * @return A read-only pointer to the Module struct, or NULL if not found.
 */

/**
 * @brief Gets a list of all registered and compiled-in modules.
 * @param[out] count A pointer to an integer that will be filled with the number of modules.
 * @param arena The memory arena, needed to initialize the module list on first call.
 * @return A constant pointer to the array of Module structs.
 */
const Module* module_get_all(int* count, struct MemoryArena* arena);

/**
 * @brief Iterates through all registered modules and applies their default settings.
 * @param config The application configuration struct to be modified.
 * @param arena The memory arena, needed to initialize the module list on first call.
 */
void module_apply_defaults(struct AppConfig* config, struct MemoryArena* arena);

/**
 * @brief Checks if a given module name corresponds to a live source requiring a watchdog.
 * @param name The name of the module to check.
 * @param arena The memory arena, needed to initialize the module list on first call.
 * @return true if the module exists and is a live source, false otherwise.
 */
bool module_is_live_source(const char* name, struct MemoryArena* arena);

/**
 * @brief Populates a buffer with the CLI options from all registered modules.
 *
 * This function iterates through all known modules and appends their argparse_option
 * structs to the destination buffer. It intercepts options for inactive modules
 * by attaching a custom callback that prints a context-aware warning, allowing
 * the parser to proceed without crashing.
 *
 * @param dest_buffer The argparse_option array to be filled.
 * @param total_opts_ptr A pointer to the running count of options in the buffer.
 * @param max_opts The capacity of the destination buffer.
 * @param active_input_type The name of the currently active input module.
 * @param active_output_type The name of the currently active output module.
 * @param arena The memory arena, needed to initialize the module list and context structs.
 */
void module_populate_cli_options(
    struct argparse_option* dest_buffer,
    int* total_opts_ptr,
    int max_opts,
    const char* active_input_type,
    const char* active_output_type,
    struct MemoryArena* arena
);

#endif // MODULE_REGISTRY_H_
