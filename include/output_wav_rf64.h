/**
 * @file output_wav_rf64.h
 * @brief Defines the public interface for the WAV/RF64 file output module.
 */

#ifndef OUTPUT_WAV_RF64_H_
#define OUTPUT_WAV_RF64_H_

#include "module.h"

/**
 * @brief Returns a pointer to the OutputModuleInterface struct that implements
 *        the output module interface for WAV/RF64 file output.
 */
OutputModuleInterface* get_wav_rf64_output_module_api(void);

#endif // OUTPUT_WAV_RF64_H_
