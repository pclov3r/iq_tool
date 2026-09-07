/**
 * @file dsp_freq_shift.h
 */

#ifndef DSP_FREQ_SHIFT_H_
#define DSP_FREQ_SHIFT_H_

#include "module.h"

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the DspModuleInterface struct that implements
 *        the DSP interface for Frequency Shifting.
 */
const struct DspModuleInterface *dsp_freq_shift_get_api(void);

#endif // DSP_FREQ_SHIFT_H_
