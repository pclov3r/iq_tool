// include/input_rawfile.h

#ifndef INPUT_RAWFILE_H_
#define INPUT_RAWFILE_H_

#include "input_source.h" // Include the generic input source interface
#include "argparse.h"     // <<< ADDED

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for raw file input.
 *
 * @return A pointer to the static InputSourceOps struct for raw files.
 */
InputSourceOps* get_raw_file_input_ops(void);

/**
 * @brief Returns the command-line options specific to the Raw File module.
 * @param count A pointer to an integer that will be filled with the number of options.
 * @return A pointer to a static array of argparse_option structs.
 */
const struct argparse_option* rawfile_get_cli_options(int* count); // <<< ADDED

#endif // INPUT_RAWFILE_H_
