/**
 * @file input_stdin.h
 */

#ifndef INPUT_STDIN_H_
#define INPUT_STDIN_H_

#include "module.h"

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for Standard Input (stdin).
 */
InputModuleInterface* input_stdin_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the stdin module.
 */
const struct argparse_option* stdin_input_get_cli_options(int* count);

#endif // INPUT_STDIN_H_
