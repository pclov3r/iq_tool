#ifndef SETUP_H_
#define SETUP_H_

#include "types.h"

// --- Function Declarations for Setup Steps ---

/**
 * @brief Resolves input and output paths based on configuration.
 *
 * On Windows, this calls platform-specific functions to get absolute Wide/UTF-8
 * paths. On POSIX systems, it sets the effective paths directly from the
 * arguments. The resolved paths are stored in the AppConfig struct.
 *
 * @param config Pointer to the AppConfig struct (modified with effective paths).
 * @return true on success, false on path resolution failure (Windows only).
 */
bool resolve_file_paths(AppConfig *config);

/**
 * @brief Opens the input WAV file using libsndfile and validates its format.
 *
 * Checks for 2 channels, supported PCM subtypes, a valid sample rate, and
 * ensures the file is not empty. Populates relevant fields in AppResources
 * (infile, sfinfo, input_bit_depth, etc.).
 *
 * @param config Pointer to the AppConfig struct (read-only).
 * @param resources Pointer to the AppResources struct to be populated.
 * @return true on success, false if file cannot be opened or format is invalid.
 */
bool open_and_validate_input_file(AppConfig *config, AppResources *resources);

/**
 * @brief Calculates the resampling ratio and validates it against acceptable limits.
 * @param config Pointer to the AppConfig struct (read-only, needs target_rate).
 * @param resources Pointer to the AppResources struct (read-only, needs sfinfo.samplerate).
 * @param out_ratio Pointer to a float where the calculated ratio will be stored.
 * @return true if the ratio is valid, false otherwise.
 */
bool calculate_and_validate_resample_ratio(AppConfig *config, AppResources *resources, float *out_ratio);

/**
 * @brief Allocates all necessary processing buffer pools.
 *
 * Calculates required sizes for each WorkItem and allocates large contiguous
 * memory pools for raw input, scaled complex data, shifted data (if needed),
 * resampled data, and final output data.
 *
 * @param config Pointer to the AppConfig struct (read-only).
 * @param resources Pointer to the AppResources struct to be populated with buffer pools.
 * @param resample_ratio The calculated resampling ratio, used for buffer sizing.
 * @return true on success, false if any allocation fails.
 */
bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio);

/**
 * @brief Creates and initializes the liquid-dsp NCO (if needed) and Resampler objects.
 * @param config Pointer to the AppConfig struct (read-only).
 * @param resources Pointer to the AppResources struct to be populated with DSP objects.
 * @param resample_ratio The calculated resampling ratio for the resampler.
 * @return true on success, false if a DSP object fails to be created.
 */
bool create_dsp_components(AppConfig *config, AppResources *resources, float resample_ratio);

/**
 * @brief Creates the thread-safe queues and mutexes for the processing pipeline.
 *
 * Initializes the free pool, input, and output queues, and the mutexes for
 * protecting progress counters and DSP objects.
 *
 * @param resources Pointer to the AppResources struct to be populated.
 * @return true on success, false if a threading component fails to be created.
 */
bool create_threading_components(AppResources *resources);

/**
 * @brief Destroys the thread-safe queues and mutexes.
 * @param resources Pointer to the AppResources struct containing the components.
 */
void destroy_threading_components(AppResources *resources);

/**
 * @brief Prints the full configuration summary to stderr.
 * @param config Pointer to the AppConfig struct.
 * @param resources Pointer to the AppResources struct.
 */
void print_configuration_summary(const AppConfig *config, const AppResources *resources);

/**
 * @brief Checks if the requested frequency shift exceeds the Nyquist frequency.
 *
 * If it does, prints a warning and prompts the user to continue (y/n).
 * This prompt is skipped if the application is outputting to stdout.
 *
 * @param config Pointer to the AppConfig struct.
 * @param resources Pointer to the AppResources struct.
 * @return true if the user chooses to continue or no warning is needed, false
 *         if the user cancels the operation.
 */
bool check_nyquist_warning(const AppConfig *config, const AppResources *resources);

/**
 * @brief Prepares the output stream (file or stdout).
 *
 * Handles opening the output file (with an overwrite prompt) or setting up
 * stdout for binary mode on Windows.
 *
 * @param config Pointer to the AppConfig struct (contains output destination info).
 * @param resources Pointer to the AppResources struct (outfile/h_outfile are set).
 * @return true on success, false on failure (e.g., cannot open file, user cancels).
 */
bool prepare_output_stream(AppConfig *config, AppResources *resources);

#endif // SETUP_H_
