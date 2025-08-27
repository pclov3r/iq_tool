/**
 * @file cli.h
 * @brief Defines the interface for the command-line argument parser.
 *
 * This module is responsible for parsing the command-line arguments provided
 * by the user, populating the main AppConfig structure, and handling requests
 * for help text or version information.
 */

#ifndef CLI_H_
#define CLI_H_

#include <stdbool.h>

// --- Forward Declarations ---
// We only use pointers to these structs in the function signatures,
// so we don't need their full definitions here. This reduces dependencies
// and improves compile times.
struct AppConfig;
struct MemoryArena;

// --- Function Declarations ---

/**
 * @brief Parses command-line arguments and populates the AppConfig struct.
 *
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @param config Pointer to the AppConfig struct to populate.
 * @param arena Pointer to the memory arena for setup-time allocations.
 * @return true on successful parsing, false on a syntax error or an invalid value format.
 */
bool parse_arguments(int argc, char *argv[], struct AppConfig *config, struct MemoryArena* arena);

/**
 * @brief Prints detailed usage instructions for the application to stderr.
 *
 * @param prog_name The name of the program (typically argv[0]).
 * @param config A pointer to the AppConfig struct (needed for presets).
 * @param arena A pointer to an initialized memory arena (needed for module list).
 */
void print_usage(const char *prog_name, struct AppConfig *config, struct MemoryArena* arena);

#endif // CLI_H_
