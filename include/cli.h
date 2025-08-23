// cli.h

#ifndef CLI_H_
#define CLI_H_

#include "types.h"
#include "memory_arena.h"

/**
 * @brief Parses command-line arguments and populates the AppConfig struct.
 *
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @param config Pointer to the AppConfig struct to populate.
 * @param arena Pointer to the memory arena for setup-time allocations.
 * @return true on successful parsing, false on a syntax error or an invalid value format.
 */
bool parse_arguments(int argc, char *argv[], AppConfig *config, MemoryArena* arena);

/**
 * @brief Prints detailed usage instructions for the application to stderr.
 *
 * @param prog_name The name of the program (typically argv[0]).
 * @param config A pointer to the AppConfig struct (needed for presets).
 * @param arena A pointer to an initialized memory arena (needed for module list).
 */
// MODIFIED: Signature updated to accept config and arena.
void print_usage(const char *prog_name, AppConfig *config, MemoryArena* arena);

#endif // CLI_H_
