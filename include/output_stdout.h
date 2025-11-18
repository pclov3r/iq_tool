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
OutputModuleInterface* get_stdout_output_module_api(void);

#endif // OUTPUT_STDOUT_H_
