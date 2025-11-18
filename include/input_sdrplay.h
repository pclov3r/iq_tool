#ifndef INPUT_SDRPLAY_H_
#define INPUT_SDRPLAY_H_

#include "module.h"
#include "argparse.h"
#include <stdint.h> // For uint8_t in get_sdrplay_device_name

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for SDRplay device input.
 */
InputModuleInterface* get_sdrplay_input_module_api(void);

/**
 * @brief Returns the command-line options specific to the SDRplay module.
 */
const struct argparse_option* sdrplay_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the SDRplay module.
 */
void sdrplay_set_default_config(struct AppConfig* config);

/**
 * @brief Helper function to get a human-readable device name from its hardware version ID.
 */
const char* get_sdrplay_device_name(uint8_t hwVer);

#endif // INPUT_SDRPLAY_H_
