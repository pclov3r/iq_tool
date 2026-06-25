/**
 * @file output_noaawx.h
 */

#ifndef OUTPUT_NOAAWX_H_
#define OUTPUT_NOAAWX_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface for the NOAAWX (Narrowband) Receiver.
 */
OutputModuleInterface* output_noaawx_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the NOAAWX output module.
 */
const struct argparse_option* output_noaawx_get_cli_options(int* count);

#endif // OUTPUT_NOAAWX_H_
