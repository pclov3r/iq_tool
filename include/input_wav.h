// include/input_wav.h

#ifndef INPUT_WAV_H_
#define INPUT_WAV_H_

#include "input_source.h" // Include the generic input source interface
#include "argparse.h"     // <<< ADDED

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for WAV file input.
 *
 * @return A pointer to the static InputSourceOps struct for WAV files.
 */
InputSourceOps* get_wav_input_ops(void);

/**
 * @brief Returns the command-line options specific to the WAV module.
 * @param count A pointer to an integer that will be filled with the number of options.
 * @return A pointer to a static array of argparse_option structs.
 */
const struct argparse_option* wav_get_cli_options(int* count); // <<< ADDED

#endif // INPUT_WAV_H_
