// input_rawfile.h

#ifndef INPUT_RAWFILE_H_
#define INPUT_RAWFILE_H_

#include "input_source.h" // Include the generic input source interface

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for raw file input.
 *
 * @return A pointer to the static InputSourceOps struct for raw files.
 */
InputSourceOps* get_raw_file_input_ops(void);

#endif // INPUT_RAWFILE_H_
