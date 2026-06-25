/**
 * @file input_airspy.h
 */

#ifndef INPUT_AIRSPY_H_
#define INPUT_AIRSPY_H_

#include "module.h"

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for Airspy device input.
 */
InputModuleInterface* input_airspy_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the Airspy module.
 */
const struct argparse_option* input_airspy_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the Airspy module.
 */
void input_airspy_set_default_config(struct AppConfig* config);

#endif // INPUT_AIRSPY_H_
