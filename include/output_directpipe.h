/**
 * @file output_directpipe.h
 * @brief Defines the public interface for the directpipe output module.
 */
#ifndef OUTPUT_DIRECTPIPE_H_
#define OUTPUT_DIRECTPIPE_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for directpipe output.
 */
OutputModuleInterface* output_directpipe_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the DirectPipe output module.
 * @param count Pointer to store the number of options returned.
 * @return Pointer to an array of argparse_option structs.
 */
const struct argparse_option* output_directpipe_get_cli_options(int* count);

#endif // OUTPUT_DIRECTPIPE_H_
