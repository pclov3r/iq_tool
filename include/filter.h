/**
 * @file filter.h
 * @brief Defines the interface for creating and managing the user-defined FIR filter chain.
 *
 * This module is responsible for designing the FIR filter based on the user's
 * command-line specifications (e.g., lowpass, highpass, passband). It handles
 * the calculation of filter taps, windowing, and combining multiple filter
 * stages into a single, efficient filter object. It can create either a
 * standard time-domain FIR filter or a more efficient frequency-domain (FFT)
 * filter depending on the configuration.
 */

#ifndef FILTER_H_
#define FILTER_H_

#include <stdbool.h>
#include "app_context.h"
#include "mem_arena.h"
#include "pipeline_types.h" // For SampleChunk
#include "utilities.h" // For OutputSummaryInfo


// --- Function Declarations ---

/**
 * @brief Creates and initializes the FIR filter(s) based on user configuration.
 *
 * This function designs the complete user-specified filter chain. All temporary
 * memory required during the filter design process (e.g., for individual stage
 * taps) is allocated from the provided memory arena. The final, combined filter
 * object is stored in the AppContext struct.
 *
 * @param config The application configuration struct containing filter requests.
 * @param app The application app struct where the final filter object will be stored.
 * @param arena The memory arena to use for all temporary allocations during design.
 * @return true on success, false on failure.
 */
bool filter_create(AppConfig* config, AppContext* app, MemoryArena* arena);

/**
 * @brief Resets the internal state of the user-defined filter object.
 *
 * This should be called upon a stream discontinuity (e.g., a hardware source overrun)
 * to clear any old data from the filter's internal buffers. This prevents
 * stale samples from corrupting the new, incoming signal.
 *
 * @param app The application app struct containing the filter object to reset.
 */
void filter_reset(DspContext* dsp);

/**
 * @brief Destroys the user-defined filter object and frees associated memory.
 *
 * @param app The application app struct containing the filter object to destroy.
 */
void filter_destroy(AppContext* app);

/**
 * @brief Applies the configured filter to a chunk of samples.
 *
 * This function encapsulates all implementation details, whether the filter
 * is FIR or FFT, symmetric or asymmetric. It handles all buffer management
 * and state (remainders) internally. The operation may be in-place or
 * out-of-place depending on the filter type.
 *
 * @param app The application app, containing filter objects and state.
 * @param item The SampleChunk containing the data to be processed.
 * @param is_post_resample A flag indicating if this is being called from the post-processor.
 * @return The number of valid output frames produced by the filter.
 */
unsigned int filter_apply(DspContext* dsp, SampleChunk* item, bool is_post_resample);

/**
 * @brief Populates the CLI options specific to the Filter module.
 *
 * @param buffer The buffer to append options to.
 * @param config The application configuration struct to bind options to.
 * @return The number of options added.
 */
int filter_populate_cli_options(struct argparse_option* buffer, struct AppConfig* config);

/**
 * @brief Adds filter summary information to the provided OutputSummaryInfo object.
 *
 * @param config The application configuration struct.
 * @param app The application context.
 * @param info The OutputSummaryInfo struct to add items to.
 */
void filter_get_summary_info(const AppConfig* config, const AppContext* app, OutputSummaryInfo* info);

#endif // FILTER_H_
