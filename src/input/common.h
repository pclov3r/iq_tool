/**
 * @file input/common.h
 */

// input/common.h

#ifndef INPUT_COMMON_H_
#define INPUT_COMMON_H_

#include "app_context.h" // Needed for AppContext
#include "utilities.h"
#include <stdatomic.h> // Needed for utility_get_time
#include <stdbool.h>

// --- Common Implementations for the InputModuleInterface Interface ---

/**
 * @brief A generic function for inputs that have a known, finite length (e.g.,
 * files).
 * @return Always returns true.
 */
static inline bool _input_has_known_length_true(void) { return true; }

/**
 * @brief A generic function for inputs that do not have a known length (e.g.,
 * live streams).
 * @return Always returns false.
 */
static inline bool _input_has_known_length_false(void) { return false; }

/**
 * @brief Updates the input heartbeat timestamp in a thread-safe manner.
 *
 * This function should be called by an Input module immediately after it
 * successfully receives data. This signals to the watchdog
 * thread that the input is alive and not deadlocked.
 *
 * @param app A pointer to the application's app.
 */
static inline void input_update_heartbeat(AppContext *app) {
  atomic_store_explicit(&app->stats.last_input_heartbeat_time,
                        utility_get_time(), memory_order_relaxed);
}

#endif // INPUT_COMMON_H_
