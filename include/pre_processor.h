#ifndef PRE_PROCESSOR_H_
#define PRE_PROCESSOR_H_

#include "app_context.h"

/**
 * @brief Applies the full chain of pre-resampling DSP operations to a sample buffer.
 *
 * This function acts as the single source of truth for the pre-processing sequence,
 * which may include DC blocking, I/Q correction, and other steps.
 *
 * @param resources A pointer to the main application resources.
 * @param samples The buffer of complex float samples to be processed in-place.
 * @param num_samples The number of samples in the buffer.
 */
void pre_processor_apply_chain(AppResources* resources, complex_float_t* samples, size_t num_samples);

#endif // PRE_PROCESSOR_H_
