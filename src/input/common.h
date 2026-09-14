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

/**
 * @brief Checks if an error has occurred in the application in a thread-safe
 * manner.
 * @param app A pointer to the application's context.
 * @return True if an error occurred, false otherwise.
 */
static inline bool input_has_error(const AppContext *app) {
  return app &&
         atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed);
}

#endif // INPUT_COMMON_H_
