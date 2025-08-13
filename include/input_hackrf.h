#ifndef INPUT_HACKRF_H_
#define INPUT_HACKRF_H_

#include <stdbool.h>
#include <hackrf.h>
#include "input_source.h"
#include "argparse.h" // <<< ADDED

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for HackRF device input.
 *
 * @return A pointer to the static InputSourceOps struct for HackRF.
 */
InputSourceOps* get_hackrf_input_ops(void);

/**
 * @brief Returns the command-line options specific to the HackRF module.
 * @param count A pointer to an integer that will be filled with the number of options.
 * @return A pointer to a static array of argparse_option structs.
 */
const struct argparse_option* hackrf_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the HackRF module.
 */
void hackrf_set_default_config(AppConfig* config);

/**
 * @brief The callback function passed to libhackrf to handle incoming samples.
 *        (Remains public as it's called by the libhackrf library).
 */
int hackrf_stream_callback(hackrf_transfer* transfer);

#endif // INPUT_HACKRF_H_
