/**
 * @file dsp_agc.h
 */

#ifndef DSP_AGC_H_
#define DSP_AGC_H_

#include "module.h"

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the DspModuleInterface struct that implements
 *        the DSP interface for Automatic Gain Control.
 */
const struct DspModuleInterface *dsp_agc_get_api(void);

#endif // DSP_AGC_H_
