#ifndef RESAMPLE_H_
#define RESAMPLE_H_

#include "types.h" // Uses AppConfig, AppResources

// --- Function Declarations ---

/**
 * @brief Initializes resources required for resampling.
 *        This includes resolving paths, opening files, checking formats,
 *        allocating buffer POOLS and WorkItems, creating/initializing queues and mutexes,
 *        and creating DSP objects (resampler, NCO). Populates the free queue.
 * @param config Pointer to the application configuration struct (read-only within function, except for effective paths).
 * @param resources Pointer to the application resources struct to be populated.
 * @return true if all resources were successfully initialized, false otherwise.
 * @note On Windows, this function resolves paths using platform functions and stores them in config.
 *       On POSIX, it sets effective paths directly from arguments in config.
 *       Handles user prompts (e.g., file overwrite, Nyquist warning) via setup helpers.
 */
bool initialize_resources(AppConfig *config, AppResources *resources);

/**
 * @brief Creates, runs, and joins the reader, processor, and writer threads.
 *        Manages the overall processing pipeline execution.
 * @param config Pointer to the application configuration struct (read-only).
 * @param resources Pointer to the initialized application resources struct.
 * @return true if processing completed successfully without errors, false otherwise.
 * @note This function blocks until all threads have completed or an error occurs.
 *       Progress updates (if not stdout) are handled within the writer thread.
 */
bool run_processing_threads(AppConfig *config, AppResources *resources);

/**
 * @brief Cleans up and releases all allocated resources.
 *        Frees memory pools, destroys DSP objects, destroys queues and mutexes,
 *        closes files/handles. Should be called AFTER run_processing_threads returns
 *        and all threads have been joined.
 * @param config Pointer to the application configuration (used for path freeing on Windows).
 * @param resources Pointer to the application resources struct.
 * @note This function is safe to call even if initialize_resources did not fully complete.
 *       It checks for NULL pointers and invalid handles before attempting cleanup.
 */
void cleanup_resources(AppConfig *config, AppResources *resources);

#endif // RESAMPLE_H_
