#ifndef OUTPUT_AM_H
#define OUTPUT_AM_H

#include "module.h"
#include "argparse.h" // Ensure argparse struct is visible

// API Getter
OutputModuleInterface* get_am_output_module_api(void);

// CLI Options Getter (Needed for module registration)
const struct argparse_option* am_get_cli_options(int* count);

#endif // OUTPUT_AM_H
