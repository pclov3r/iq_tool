/**
 * @file input_hydrasdr.h
 */

#ifndef INPUT_HYDRASDR_H_
#define INPUT_HYDRASDR_H_

#include "module.h"

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for HydraSDR device input.
 */
InputModuleInterface* input_hydrasdr_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the HydraSDR module.
 */
const struct argparse_option* hydrasdr_input_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the HydraSDR module.
 */
void hydrasdr_set_default_config(struct AppConfig* config);

#endif // INPUT_HYDRASDR_H_
