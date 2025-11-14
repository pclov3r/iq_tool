// input_common.h

#ifndef INPUT_COMMON_H_
#define INPUT_COMMON_H_

#include <stdbool.h>
#include "app_context.h" // Needed for AppResources
#include "utils.h"       // Needed for get_monotonic_time_sec

// --- Common Implementations for the ModuleInterface Interface ---

/**
 * @brief A generic function for sources that have a known, finite length (e.g., files).
 * @return Always returns true.
 */
static inline bool _input_source_has_known_length_true(void) {
    return true;
}

/**
 * @brief A generic function for sources that do not have a known length (e.g., live streams).
 * @return Always returns false.
 */
static inline bool _input_source_has_known_length_false(void) {
    return false;
}

/**
 * @brief Updates the SDR heartbeat timestamp in a thread-safe manner.
 *
 * This function should be called by an SDR module immediately after it
 * successfully receives data from the hardware. This signals to the watchdog
 * thread that the SDR is alive and not deadlocked.
 *
 * @param resources A pointer to the application's resources.
 */
static inline void sdr_input_update_heartbeat(AppResources* resources) {
    pthread_mutex_lock(&resources->progress_mutex);
    resources->last_sdr_heartbeat_time = get_monotonic_time_sec();
    pthread_mutex_unlock(&resources->progress_mutex);
}

#endif // INPUT_COMMON_H_
