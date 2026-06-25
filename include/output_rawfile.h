/**
 * @file output_rawfile.h
 * @brief Defines the public interface for the raw file output module.
 */
#ifndef OUTPUT_RAWFILE_H_
#define OUTPUT_RAWFILE_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for raw file output.
 */
OutputModuleInterface* output_rawfile_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the Raw File output module.
 * @param count Pointer to store the number of options returned.
 * @return Pointer to an array of argparse_option structs.
 */
const struct argparse_option* output_rawfile_get_cli_options(int* count);

#endif // OUTPUT_RAW_FILE_H_
