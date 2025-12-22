#ifndef INPUT_AIRSPYHF_H_
#define INPUT_AIRSPYHF_H_

#include "module.h"
#include "argparse.h"

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for Airspy HF+ device input.
 */
InputModuleInterface* input_airspyhf_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the Airspy HF+ module.
 */
const struct argparse_option* airspyhf_input_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the Airspy HF+ module.
 */
void airspyhf_set_default_config(struct AppConfig* config);

#endif // INPUT_AIRSPYHF_H_
