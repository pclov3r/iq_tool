/**
 * @file pre_processor.h
 * @brief Defines the interface for the pre-resampling DSP processing stage.
 */

#ifndef PRE_PROCESSOR_H_
#define PRE_PROCESSOR_H_

#include "app_context.h"

/**
 * @brief Applies the full chain of pre-resampling DSP operations to a sample buffer.
 *
 * This function acts as the single source of truth for the pre-processing sequence,
 * which includes sample format conversion, DC blocking, I/Q correction, frequency
 * shifting, and pre-resample filtering.
 *
 * @param resources A pointer to the main application resources.
 * @param item The SampleChunk containing the data to be processed. The operation
 *             is performed on the raw_input_data buffer, and the results are
 *             placed in the complex_pre_resample_data buffer. The frames_read
 *             field may be modified by filtering.
 */
void pre_processor_apply_chain(AppResources* resources, SampleChunk* item);

/**
 * @brief Resets the state of all stateful DSP modules in the pre-processing chain.
 *
 * This should be called upon a stream discontinuity to prevent stale data from
 * corrupting new samples.
 *
 * @param resources A pointer to the main application resources.
 */
void pre_processor_reset(AppResources* resources);

#endif // PRE_PROCESSOR_H_
