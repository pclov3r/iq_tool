/**
 * @file post_processor.h
 * @brief Defines the interface for the post-resampling DSP processing stage.
 */

#ifndef POST_PROCESSOR_H_
#define POST_PROCESSOR_H_

#include "app_context.h"

/**
 * @brief Applies the full chain of post-resampling DSP operations to a sample buffer.
 *
 * This function acts as the single source of truth for the post-processing sequence,
 * which may include user-defined filtering, frequency shifting, and final sample
 * format conversion.
 *
 * @param app A pointer to the main application app.
 * @param item The SampleChunk containing the data to be processed. The operation
 *             is performed on the complex_resampled_data buffer, and the final
 *             results are placed in the final_output_data buffer. The
 *             frames_to_write field may be modified by filtering.
 */
void post_processor_apply_chain(DspContext* dsp, SampleChunk* item);

/**
 * @brief Resets the state of all stateful DSP modules in the post-processing chain.
 *
 * This should be called upon a stream discontinuity to prevent stale data from
 * corrupting new samples.
 *
 * @param app A pointer to the main application app.
 */
void post_processor_reset(DspContext* dsp);

#endif // POST_PROCESSOR_H_
