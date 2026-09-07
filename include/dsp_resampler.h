/**
 * @file dsp_resampler.h
 */

#ifndef DSP_RESAMPLER_H_
#define DSP_RESAMPLER_H_

#include "module.h"

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the DspModuleInterface struct that implements
 *        the DSP interface for Resampling.
 */
const struct DspModuleInterface *dsp_resampler_get_api(void);

#endif // DSP_RESAMPLER_H_
