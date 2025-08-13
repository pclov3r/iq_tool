// src/input_common.h

#ifndef INPUT_COMMON_H_
#define INPUT_COMMON_H_

#include <stdbool.h>

// --- Common Implementations for the InputSourceOps Interface ---

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

#endif // INPUT_COMMON_H_
