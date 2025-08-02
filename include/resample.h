#ifndef RESAMPLE_H_
#define RESAMPLE_H_

#include "types.h"

/**
 * @brief Creates, runs, and joins the reader, processor, and writer threads.
 *
 * This function is the entry point to the core processing engine. It starts
 * the multi-threaded pipeline and blocks until all threads have completed their
 * work or a fatal error occurs.
 *
 * @param config Pointer to the application configuration struct.
 * @param resources Pointer to the fully initialized application resources struct.
 * @return true if processing completed successfully without errors, false otherwise.
 */
bool run_processing_threads(AppConfig *config, AppResources *resources);

#endif // RESAMPLE_H_
