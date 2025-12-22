#ifndef OUTPUT_WFM_H_
#define OUTPUT_WFM_H_

#include "module.h"
#include "argparse.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface for the WFM Stereo Receiver.
 */
OutputModuleInterface* output_wfm_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the WFM output module.
 */
const struct argparse_option* wfm_output_get_cli_options(int* count);

#endif // OUTPUT_WFM_H_
