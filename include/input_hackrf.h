#ifndef INPUT_HACKRF_H_
#define INPUT_HACKRF_H_

#include "input_source.h"
#include "argparse.h"

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for HackRF device input.
 */
InputSourceOps* get_hackrf_input_ops(void);

/**
 * @brief Returns the command-line options specific to the HackRF module.
 */
const struct argparse_option* hackrf_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the HackRF module.
 */
void hackrf_set_default_config(struct AppConfig* config);

#endif // INPUT_HACKRF_H_
