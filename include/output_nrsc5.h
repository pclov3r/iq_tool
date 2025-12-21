/**
 * @file output_nrsc5.h
 * @brief Defines the public interface for the NRSC5 (HD Radio) output module.
 */

#ifndef OUTPUT_NRSC5_H_
#define OUTPUT_NRSC5_H_

#include "module.h"
#include "argparse.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for NRSC5 playback.
 */
OutputModuleInterface* get_nrsc5_output_module_api(void);

/**
 * @brief Returns the command-line options specific to the NRSC5 output module.
 *
 * @param[out] count A pointer to an integer that will be filled with the number of options.
 * @return A pointer to the array of argparse_option structs.
 */
const struct argparse_option* nrsc5_output_get_cli_options(int* count);

#endif // OUTPUT_NRSC5_H_
