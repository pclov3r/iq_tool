// input_wav.h
#ifndef INPUT_WAV_H_
#define INPUT_WAV_H_

#include "input_source.h" // Include the generic input source interface

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for WAV file input.
 *
 * @return A pointer to the static InputSourceOps struct for WAV files.
 */
InputSourceOps* get_wav_input_ops(void);

#endif // INPUT_WAV_H_
