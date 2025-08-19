// src/filter.h

#ifndef FILTER_H_
#define FILTER_H_

#include "types.h"
#include <stdbool.h>

/**
 * @brief Creates and initializes the FIR filter(s) based on user configuration.
 *
 * This function creates the user-specified filter. If the configuration involves
 * significant downsampling AND the user has specified a filter, it will ALSO create
 * a separate, preceding anti-aliasing low-pass filter to prevent aliasing.
 *
 * @param config The application configuration struct.
 * @param resources The application resources struct where the filter object(s) will be stored.
 * @return true on success, false on failure.
 */
bool filter_create(AppConfig* config, AppResources* resources);

/**
 * @brief Destroys all FIR filter objects and frees associated memory.
 * @param resources The application resources struct.
 */
void filter_destroy(AppResources* resources);

#endif // FILTER_H_
