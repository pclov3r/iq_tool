#ifndef SPECTRUM_SHIFT_H_
#define SPECTRUM_SHIFT_H_

#include "types.h"
#include <stdbool.h>

/**
 * @brief An enum to specify at which stage of the pipeline the shift should be applied.
 */
typedef enum {
    SHIFT_STAGE_PRE_RESAMPLE,
    SHIFT_STAGE_POST_RESAMPLE
} ShiftApplyStage;

/**
 * @brief Creates and configures the NCO (frequency shifter) based on user arguments.
 *
 * This function reads the frequency shift settings from the AppConfig struct,
 * calculates the required shift, and creates the liquid-dsp NCO object if a
 * shift is necessary. The created object and the final shift value are stored
 * in the AppResources struct.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources where the NCO will be stored.
 * @return true on success or if no shift is needed, false on failure (e.g., metadata missing, NCO creation fails).
 */
bool shift_create_nco(AppConfig *config, AppResources *resources);

/**
 * @brief Applies the frequency shift to a block of complex samples.
 *
 * This function checks if a shift is required at the specified pipeline stage
 * and, if so, applies it to the appropriate buffer in the WorkItem.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources containing the NCO.
 * @param item Pointer to the current WorkItem containing the data buffers.
 * @param stage The pipeline stage (pre- or post-resample) to check for application.
 */
void shift_apply(AppConfig *config, AppResources *resources, WorkItem *item, ShiftApplyStage stage);

/**
 * @brief Checks if the configured frequency shift exceeds the Nyquist frequency and warns the user.
 *
 * If a large shift is requested, this function prints a warning about potential aliasing
 * and prompts the user to continue, unless outputting to stdout.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @return true if the user chooses to continue or no warning is needed, false if the user cancels.
 */
bool shift_check_nyquist_warning(const AppConfig *config, const AppResources *resources);

/**
 * @brief Resets the internal state of the NCO.
 * @param resources Pointer to the application resources containing the NCO.
 */
void shift_reset_nco(AppResources *resources);

/**
 * @brief Destroys the NCO object if it was created.
 * @param resources Pointer to the application resources containing the NCO.
 */
void shift_destroy_nco(AppResources *resources);

#endif // SPECTRUM_SHIFT_H_
