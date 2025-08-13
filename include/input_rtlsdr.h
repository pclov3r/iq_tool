#ifndef INPUT_RTLSDR_H_
#define INPUT_RTLSDR_H_

#include "input_source.h"
#include <rtl-sdr.h>
#include "argparse.h" // <<< ADDED

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for RTL-SDR device input.
 *
 * @return A pointer to the static InputSourceOps struct for RTL-SDR.
 */
InputSourceOps* get_rtlsdr_input_ops(void);

/**
 * @brief Returns the command-line options specific to the RTL-SDR module.
 * @param count A pointer to an integer that will be filled with the number of options.
 * @return A pointer to a static array of argparse_option structs.
 */
const struct argparse_option* rtlsdr_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the RTL-SDR module.
 */
void rtlsdr_set_default_config(AppConfig* config);

#endif // INPUT_RTLSDR_H_
