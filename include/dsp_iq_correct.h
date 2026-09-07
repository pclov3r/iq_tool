/**
 * @file dsp_iq_correct.h
 */

#ifndef DSP_IQ_CORRECT_H_
#define DSP_IQ_CORRECT_H_

#include "module.h"

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the DspModuleInterface struct that implements
 *        the DSP interface for IQ Correction.
 */
const struct DspModuleInterface *dsp_iq_correct_get_api(void);

#endif // DSP_IQ_CORRECT_H_
