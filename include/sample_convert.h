// sample_convert.h
#ifndef SAMPLE_CONVERT_H_
#define SAMPLE_CONVERT_H_

#include "types.h" // For AppConfig, AppResources, WorkItem

/**
 * @brief Selects the appropriate sample conversion function based on the input format
 *        and user options, then stores it in the AppResources struct. This should be
 *        called once during initialization.
 *
 * @param config Pointer to the application configuration, used to check for flags
 *               like 'native_8bit_path'.
 * @param resources Pointer to the application resources. The input_pcm_format field
 *                  must be set before calling this function. The 'converter' member
 *                  will be populated with the correct function pointer.
 */
void setup_sample_converter(AppConfig *config, AppResources *resources);

/**
 * @brief Dispatches to the pre-selected function to convert raw input samples
 *        into a standardized floating-point complex format.
 *
 * This function is called in the hot loop and delegates the actual work to the
 * function pointer set by setup_sample_converter().
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @param item Pointer to the WorkItem containing the raw input and complex buffers.
 * @return true on successful conversion, false if a fatal error occurred.
 */
bool convert_raw_input_to_complex(AppConfig *config, AppResources *resources, WorkItem *item);

/**
 * @brief Converts the final complex float samples into the specified output byte format.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @param item Pointer to the WorkItem containing the final complex data and the output buffer.
 * @param num_frames The number of complex frames to convert.
 */
void convert_complex_to_output_format(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int num_frames);

#endif // SAMPLE_CONVERT_H_
