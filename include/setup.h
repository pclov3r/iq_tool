#ifndef SETUP_H_
#define SETUP_H_

#include "types.h" // Needs AppConfig, AppResources definitions

// --- Function Declarations for Setup Steps ---

/**
 * @brief Resolves input and output paths based on configuration.
 *        On Windows, calls platform functions to get absolute Wide/UTF-8 paths.
 *        On POSIX, sets effective paths directly from arguments.
 *        Stores resolved paths in the AppConfig struct.
 * @param config Pointer to the AppConfig struct (modified with effective paths).
 * @return true on success, false on path resolution failure (Windows only).
 */
bool resolve_file_paths(AppConfig *config);

/**
 * @brief Opens the input WAV file using libsndfile and validates its format.
 *        Checks channels, supported PCM subtypes, sample rate, and for empty files.
 *        Populates relevant fields in AppResources (infile, sfinfo, input_bit_depth, etc.).
 * @param config Pointer to the AppConfig struct (read-only, used for path and flags).
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
 * @brief Allocates all necessary processing buffer pools based on configuration and NUM_BUFFERS.
 *        Calculates required sizes per WorkItem and allocates large contiguous pools.
 *        Does NOT initialize the WorkItems themselves (done later).
 * @param config Pointer to the AppConfig struct (read-only, needs mode, freq_shift info).
 * @param resources Pointer to the AppResources struct to be populated with buffer pools.
 * @param resample_ratio The calculated resampling ratio needed for buffer sizing.
 * @return true on success, false if any allocation fails or sizes are unreasonable.
 */
bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio);

/**
 * @brief Creates and initializes the liquid-dsp NCO (if needed) and Resampler objects.
 * @param config Pointer to the AppConfig struct (read-only, needs freq_shift info).
 * @param resources Pointer to the AppResources struct (read-only for rates, modified with DSP objects).
 * @param resample_ratio The calculated resampling ratio for the resampler.
 * @return true on success, false if DSP object creation fails.
 */
bool create_dsp_components(AppConfig *config, AppResources *resources, float resample_ratio);

/**
 * @brief Prints the full configuration summary to stderr. (Only called if not outputting to stdout).
 * @param config Pointer to the AppConfig struct.
 * @param resources Pointer to the AppResources struct.
 * @param resample_ratio The calculated resampling ratio.
 */
void print_configuration_summary(const AppConfig *config, const AppResources *resources, float resample_ratio);

/**
 * @brief Checks if the requested frequency shift exceeds the Nyquist frequency.
 *        If it does, prints a warning and prompts the user to continue (y/n).
 *        (Only prompts if not outputting to stdout).
 * @param config Pointer to the AppConfig struct.
 * @param resources Pointer to the AppResources struct.
 * @return true if the user chooses to continue or no warning/prompt was needed, false if the user cancels.
 */
bool check_nyquist_warning(const AppConfig *config, const AppResources *resources);


/**
 * @brief Prepares the output stream (file or stdout).
 *        Handles opening the output file (with overwrite prompt) or setting up stdout binary mode.
 * @param config Pointer to the AppConfig struct (contains output destination info).
 * @param resources Pointer to the AppResources struct (outfile and h_outfile are set).
 * @return true on success, false on failure (e.g., cannot open file, user cancels overwrite).
 */
bool prepare_output_stream(AppConfig *config, AppResources *resources);


#endif // SETUP_H_
