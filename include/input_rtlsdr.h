#ifndef INPUT_RTLSDR_H_
#define INPUT_RTLSDR_H_

#include "module.h"
#include "argparse.h"

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for RTL-SDR device input.
 */
InputModuleInterface* get_rtlsdr_input_module_api(void);

/**
 * @brief Returns the command-line options specific to the RTL-SDR module.
 */
const struct argparse_option* rtlsdr_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the RTL-SDR module.
 */
void rtlsdr_set_default_config(struct AppConfig* config);

#endif // INPUT_RTLSDR_H_
