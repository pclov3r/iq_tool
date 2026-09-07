/**
 * @file dsp_dcblock.h
 */

#ifndef DSP_DCBLOCK_H_
#define DSP_DCBLOCK_H_

#include "module.h"

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the DspModuleInterface struct that implements
 *        the DSP interface for the DC Blocker.
 */
const struct DspModuleInterface *dsp_dcblock_get_api(void);

#endif // DSP_DCBLOCK_H_
