/**
 * @file output_raw.h
 * @brief Defines the public interface for the raw file output module.
 */

#ifndef OUTPUT_RAW_FILE_H_
#define OUTPUT_RAW_FILE_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for raw file output.
 */
OutputModuleInterface* get_raw_file_output_module_api(void);

#endif // OUTPUT_RAW_FILE_H_
