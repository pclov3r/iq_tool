/**
 * @file setup.h
 * @brief Declares the high-level functions for application initialization and cleanup.
 *
 * This module orchestrates the entire setup and teardown process of the
 * application. It calls the various sub-modules in the correct order to
 * resolve file paths, initialize hardware, create DSP objects, allocate
 * memory pools, and prepare all app before the processing threads
 * are launched. It also manages the corresponding cleanup sequence.
 */

#ifndef SETUP_H_
#define SETUP_H_

#include <stdbool.h>
#include "app_context.h" // Include the full definitions

// --- Function Declarations for Setup Steps ---

/**
 * @brief The main entry point for application initialization.
 *
 * This function orchestrates the entire setup sequence after command-line
 * arguments have been parsed.
 *
 * @param config The application configuration.
 * @param app The application app struct to be populated.
 * @return true on successful initialization, false on any failure.
 */
bool initialize_application(struct AppConfig *config, struct AppContext* app);

/**
 * @brief The main entry point for application cleanup.
 *
 * This function orchestrates the teardown of all app in the reverse
 * order of their creation.
 *
 * @param config The application configuration.
 * @param app The application app struct to be cleaned up.
 */
void cleanup_application(struct AppConfig *config, struct AppContext* app);

/**
 * @brief Resolves relative input/output file paths to absolute paths.
 *
 * This is particularly important on Windows to handle different character sets
 * and ensure file operations are unambiguous.
 *
 * @param config The application configuration struct containing the path arguments.
 * @param app The application app, needed for the memory arena on POSIX.
 * @return true on success, false on failure.
 */
bool resolve_file_paths(struct AppConfig *config, struct AppContext* app);

/**
 * @brief Calculates the resampling ratio and validates it is within a sane range.
 *
 * Also calculates the expected number of output frames for file-based sources.
 *
 * @param config The application configuration.
 * @param app The application app.
 * @param[out] out_ratio A pointer to a float to store the calculated ratio.
 * @return true on success, false if the ratio is invalid.
 */
bool calculate_and_validate_resample_ratio(struct AppConfig *config, struct AppContext* app, float *out_ratio);

/**
 * @brief Determines if the user-defined filter should be applied before or after resampling.
 *
 * This is an important optimization. If downsampling, it checks if the filter's
 * passband is within the Nyquist frequency of the *output* rate. If so, it's
 * more efficient to filter after resampling.
 *
 * @param config The application configuration.
 * @param app The application app.
 * @return true on success, false if the filter configuration is invalid for the output rate.
 */
bool validate_and_configure_filter_stage(struct AppConfig *config, struct AppContext* app);

/**
 * @brief Allocates all the main memory pools for the processing pipeline.
 *
 * This includes the large, contiguous block for all `SampleChunk` data buffers
 * and the pool of `SampleChunk` structs themselves.
 *
 * @param config The application configuration.
 * @param app The application app.
 * @param resample_ratio The calculated resampling ratio, needed to size buffers correctly.
 * @return true on success, false on memory allocation failure.
 */
bool allocate_processing_buffers(struct AppConfig *config, struct AppContext* app, float resample_ratio);

/**
 * @brief Creates and initializes all non-pipeline threading components.
 * @param config The application configuration.
 * @param app The application app struct.
 * @return true on success, false on failure.
 */
bool create_threading_components(struct AppConfig *config, struct AppContext* app);

/**
 * @brief Prints the final, resolved configuration summary to stderr.
 * @param config The application configuration.
 * @param app The application app.
 */
void print_configuration_summary(const struct AppConfig *config, const struct AppContext* app);

/**
 * @brief Prepares the final output stream by initializing the file writer and opening the file/stream.
 * @param config The application configuration.
 * @param app The application app.
 * @return true on success, false on failure.
 */
bool prepare_output_stream(struct AppConfig *config, struct AppContext* app);

#endif // SETUP_H_
