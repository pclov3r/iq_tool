#ifndef CLI_H_
#define CLI_H_

#include "types.h"

/**
 * @brief Parses command-line arguments and populates the AppConfig struct.
 *
 * This function iterates through the command-line arguments, populates the
 * config struct with user-provided values, and sets appropriate defaults for
 * any unspecified options. It performs basic validation on the format of
 * individual arguments (e.g., ensuring a number is valid) but does not
 * validate the logical relationships between arguments.
 *
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @param config Pointer to the AppConfig struct to populate.
 * @return true on successful parsing, false on a syntax error (e.g., missing
 *         argument for an option) or an invalid value format.
 */
bool parse_arguments(int argc, char *argv[], AppConfig *config);

/**
 * @brief Prints detailed usage instructions for the application to stderr.
 * @param prog_name The name of the program (typically argv[0]).
 */
void print_usage(const char *prog_name);

#endif // CLI_H_
