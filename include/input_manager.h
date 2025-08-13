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
 *
 * This function acts as a factory, returning the set of function pointers
 * for the requested input source (e.g., "wav", "sdrplay").
 *
 * @param name The name of the input source, typically from the --input argument.
 * @return A pointer to the corresponding static InputSourceOps struct, or NULL if
 *         the name is not recognized or the module is not compiled in.
 */
InputSourceOps* get_input_ops_by_name(const char* name);

/**
 * @brief Gets a list of all registered and compiled-in input modules.
 *
 * This allows generic code (like the CLI parser) to iterate over all available
 * modules without needing to know their specific names at compile time.
 *
 * @param count A pointer to an integer that will be filled with the number of modules.
 * @return A pointer to a static array of InputModule structs.
 */
const InputModule* get_all_input_modules(int* count);

/**
 * @brief Iterates through all registered modules and applies their default settings.
 */
void input_manager_apply_defaults(AppConfig* config);

/**
 * @brief Checks if a given input type name corresponds to an SDR device.
 * @param name The name of the input type (e.g., "rtlsdr", "wav").
 * @return true if the name is a known SDR type, false otherwise.
 */
bool is_sdr_input(const char* name);

#endif // INPUT_MANAGER_H_
