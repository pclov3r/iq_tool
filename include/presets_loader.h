#ifndef PRESETS_LOADER_H_
#define PRESETS_LOADER_H_

#include "types.h" // We need this for the AppConfig struct definition

/**
 * @brief Loads preset definitions from a text file, searching common locations.
 *
 * This function searches for and reads the specified file, parses the presets
 * defined within it, and stores them in a dynamically allocated array inside
 * the AppConfig struct. It includes validation to handle common errors and
 * prevent resource exhaustion from malicious files.
 *
 * @param config A pointer to the AppConfig struct where the loaded presets will be stored.
 * @return true on success (even if no file is found or a conflict is warned), false on a fatal
 *         error (e.g., memory allocation failure).
 */
bool presets_load_from_file(AppConfig* config);

/**
 * @brief Frees the memory allocated for the presets loaded from the file.
 *
 * This should be called at the end of the program to clean up the dynamically
 * allocated strings and the preset array itself.
 *
 * @param config A pointer to the AppConfig struct containing the presets to free.
 */
void presets_free_loaded(AppConfig* config);

#endif // PRESETS_LOADER_H_
