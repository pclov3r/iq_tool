/**
 * @file output_stdout.h
 * @brief Defines the public interface for the stdout output module.
 */
#ifndef OUTPUT_STDOUT_H_
#define OUTPUT_STDOUT_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for stdout output.
 */
OutputModuleInterface* output_stdout_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the Stdout output module.
 * @param count Pointer to store the number of options returned.
 * @return Pointer to an array of argparse_option structs.
 */
const struct argparse_option* output_stdout_get_cli_options(int* count);

#endif // OUTPUT_STDOUT_H_
