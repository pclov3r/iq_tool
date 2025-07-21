#ifndef CLI_H_
#define CLI_H_

#include "types.h" // Uses AppConfig struct

// --- Function Declarations ---

/**
 * @brief Parses command line arguments and populates the config struct.
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @param config Pointer to the AppConfig struct to populate.
 * @return true on success, false on parsing error or invalid arguments.
 * @note This function stores the raw filename arguments provided by the user.
 *       Path resolution happens later in initialize_resources. It sets the
 *       appropriate target_rate and default scale_value based on the chosen mode.
 */
bool parse_arguments(int argc, char *argv[], AppConfig *config);

/**
 * @brief Prints usage instructions to stderr.
 * @param prog_name The name of the program (argv[0]).
 */
void print_usage(const char *prog_name);

#endif // CLI_H_

