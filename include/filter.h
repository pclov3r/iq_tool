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
#include "app_context.h"  // Provides AppConfig and AppResources
#include "memory_arena.h" // Provides MemoryArena

// --- Function Declarations ---

/**
 * @brief Creates and initializes the FIR filter(s) based on user configuration.
 *
 * This function designs the complete user-specified filter chain. All temporary
 * memory required during the filter design process (e.g., for individual stage
 * taps) is allocated from the provided memory arena. The final, combined filter
 * object is stored in the AppResources struct.
 *
 * @param config The application configuration struct containing filter requests.
 * @param resources The application resources struct where the final filter object will be stored.
 * @param arena The memory arena to use for all temporary allocations during design.
 * @return true on success, false on failure.
 */
bool filter_create(AppConfig* config, AppResources* resources, MemoryArena* arena);

/**
 * @brief Resets the internal state of the user-defined filter object.
 *
 * This should be called upon a stream discontinuity (e.g., an SDR overrun)
 * to clear any old data from the filter's internal buffers. This prevents
 * stale samples from corrupting the new, incoming signal.
 *
 * @param resources The application resources struct containing the filter object to reset.
 */
void filter_reset(AppResources* resources);

/**
 * @brief Destroys the user-defined filter object and frees associated memory.
 *
 * @param resources The application resources struct containing the filter object to destroy.
 */
void filter_destroy(AppResources* resources);

#endif // FILTER_H_
