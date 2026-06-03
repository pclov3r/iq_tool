/**
 * @file output_nfm.h
 */

#ifndef OUTPUT_NFM_H_
#define OUTPUT_NFM_H_

#include "module.h"
#include "argparse.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface for the NFM (Narrowband) Receiver.
 */
OutputModuleInterface* output_nfm_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the NFM output module.
 */
const struct argparse_option* nfm_output_get_cli_options(int* count);

#endif // OUTPUT_NFM_H_
