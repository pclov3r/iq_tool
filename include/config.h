// src/config.h

#ifndef CONFIG_H_
#define CONFIG_H_

#include "types.h"

/**
 * @brief Validates that the user has not specified conflicting output destinations.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_output_destination(AppConfig *config);

/**
 * @brief Resolves presets and validates the final output format choices.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_output_type_and_sample_format(AppConfig *config);

/**
 * @brief Validates and processes all user-defined filter arguments.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_filter_options(AppConfig *config);

/**
 * @brief Resolves and validates all frequency shifting options.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool resolve_frequency_shift_options(AppConfig *config);

/**
 * @brief Validates I/Q correction dependencies.
 * @param config The application configuration struct.
 * @return true if valid, false otherwise.
 */
bool validate_iq_correction_options(AppConfig *config);

/**
 * @brief Performs high-level validation, checking for logical conflicts between different options.
 * @param config The application configuration struct.
 * @return true if the configuration is logically consistent, false otherwise.
 */
bool validate_logical_consistency(AppConfig *config);

#endif // CONFIG_H_
