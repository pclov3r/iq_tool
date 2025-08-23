#ifndef FILTER_H_
#define FILTER_H_

#include "types.h" // <-- MODIFIED: Include types for MemoryArena definition
#include <stdbool.h>

/**
 * @brief Creates and initializes the FIR filter(s) based on user configuration.
 *
 * This function creates the user-specified filter. All temporary memory required
 * during the filter design process is allocated from the provided memory arena.
 *
 * @param config The application configuration struct.
 * @param resources The application resources struct where the final filter object will be stored.
 * @param arena The memory arena to use for temporary allocations.
 * @return true on success, false on failure.
 */
bool filter_create(AppConfig* config, AppResources* resources, MemoryArena* arena);

/**
 * @brief Destroys all FIR filter objects and frees associated memory.
 * @param resources The application resources struct.
 */
void filter_destroy(AppResources* resources);

#endif // FILTER_H_
