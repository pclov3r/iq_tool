/**
 * @file input_bladerf.h
 */

#ifndef INPUT_BLADERF_H_
#define INPUT_BLADERF_H_

#include "module.h"

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for BladeRF device input.
 */
InputModuleInterface* input_bladerf_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the BladeRF module.
 */
const struct argparse_option* bladerf_input_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the BladeRF module.
 */
void bladerf_set_default_config(struct AppConfig* config);

#endif // INPUT_BLADERF_H_
