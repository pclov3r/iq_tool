/**
 * @file output_am.h
 */

#ifndef OUTPUT_AM_H_
#define OUTPUT_AM_H_

#include "module.h"

// API Getter
OutputModuleInterface* output_am_get_module_api(void);

// CLI Options Getter (Needed for module registration)
const struct argparse_option* am_output_get_cli_options(int* count);

#endif // OUTPUT_AM_H
