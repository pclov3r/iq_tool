/**
 * @file output_wav_rf64.h
 * @brief Defines the public interface for the WAV/RF64 file output module.
 */
#ifndef OUTPUT_WAV_RF64_H_
#define OUTPUT_WAV_RF64_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for WAV RF64 file output.
 */
OutputModuleInterface* output_wav_rf64_get_module_api(void);

/**
 * @brief Returns the command-line options specific to the WAV RF64 output module.
 * @param count Pointer to store the number of options returned.
 * @return Pointer to an array of argparse_option structs.
 */
const struct argparse_option* output_wav_rf64_get_cli_options(int* count);

#endif // OUTPUT_WAV_RF64_H_
