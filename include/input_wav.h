/**
 * @file input_wav.h
 */

// include/input_wav.h

#ifndef INPUT_WAV_H_
#define INPUT_WAV_H_

#include "module.h"

/**
 * @brief Returns a pointer to the InputModuleInterface struct that implements
 *        the input source interface for WAV file input.
 */
InputModuleInterface* input_wav_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the WAV module.
 */
const struct argparse_option* wav_input_get_cli_options(int* count);

#endif // INPUT_WAV_H_
