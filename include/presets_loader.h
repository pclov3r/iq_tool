#ifndef PRESETS_LOADER_H_
#define PRESETS_LOADER_H_

#include "types.h" // <-- MODIFIED: Include types.h to get definitions

// --- MODIFIED: All forward declarations are removed ---

/**
 * @brief Loads preset definitions from a text file, searching common locations.
 *
 * @param config A pointer to the AppConfig struct where the loaded presets will be stored.
 * @param arena A pointer to the memory arena to use for all allocations.
 * @return true on success, false on a fatal error.
 */
bool presets_load_from_file(AppConfig* config, MemoryArena* arena);

#endif // PRESETS_LOADER_H_
