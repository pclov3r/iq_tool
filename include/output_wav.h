/**
 * @file output_wav.h
 * @brief Defines the public interface for the WAV file output module.
 */
#ifndef OUTPUT_WAV_H_
#define OUTPUT_WAV_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for WAV file output.
 */
OutputModuleInterface* output_wav_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the WAV output module.
 * @param count Pointer to store the number of options returned.
 * @return Pointer to an array of argparse_option structs.
 */
const struct argparse_option* wav_output_get_cli_options(int* count);

#endif // OUTPUT_WAV_H_
