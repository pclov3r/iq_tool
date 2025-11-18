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
OutputModuleInterface* get_wav_output_module_api(void);

#endif // OUTPUT_WAV_H_
