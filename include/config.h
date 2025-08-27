/**
 * @file config.h
 * @brief Defines the interface for post-parsing validation of the AppConfig structure.
 *
 * This module contains a series of validation functions that are called after
 * the initial command-line parsing is complete. These functions are responsible
 * for checking for logical inconsistencies between options, resolving presets,
 * and ensuring the final configuration is valid and ready for use by the
 * processing pipeline.
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdbool.h>

// --- Forward Declaration ---
// The functions in this module only need a pointer to the AppConfig struct,
// so we use a forward declaration to avoid including the full app_context.h.
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Validates that the user has not specified conflicting output destinations.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_output_destination(struct AppConfig *config);

/**
 * @brief Resolves presets and validates the final output format choices.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_output_type_and_sample_format(struct AppConfig *config);

/**
 * @brief Validates and processes all user-defined filter arguments.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_filter_options(struct AppConfig *config);

/**
 * @brief Resolves and validates all frequency shifting options.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool resolve_frequency_shift_options(struct AppConfig *config);

/**
 * @brief Validates I/Q correction dependencies.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_iq_correction_options(struct AppConfig *config);

/**
 * @brief Performs high-level validation, checking for logical conflicts between different options.
 * @param config The application configuration struct.
 * @return true if the configuration is logically consistent, false otherwise.
 */
bool validate_logical_consistency(struct AppConfig *config);

#endif // CONFIG_H_
