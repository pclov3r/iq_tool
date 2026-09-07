/**
 * @file dsp_filter.h
 */

#ifndef DSP_FILTER_H_
#define DSP_FILTER_H_

#include "module.h"

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the DspModuleInterface struct that implements
 *        the DSP interface for Filtering.
 */
const struct DspModuleInterface *dsp_filter_get_api(void);

#endif // DSP_FILTER_H_
