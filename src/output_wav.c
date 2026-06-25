/**
 * @file output_wav.c
 * @brief Implements the standard WAV file output module.
 *
 * This file is a lightweight wrapper around the common WAV writing logic.
 * Its only job is to specify the SF_FORMAT_WAV flag during initialization
 * and provide the correct summary information. This format has a 4GB file
 * size limit; for larger files, the 'wav-rf64' output module should be used.
 */

#include "output_wav.h"
#include "output_wav_common.h" // Include the shared implementation
#include "utilities.h"             // For utility_add_summary_item
#include <sndfile.h>           // For the SF_FORMAT_WAV constant

/**
 * @brief Initializes the WAV writer by calling the common initializer.
 *
 * This function's sole responsibility is to pass the specific format flag
 * for standard WAV files to the shared initialization logic.
 */
static bool output_wav_initialize(ModuleContext* context) {
    // Call the common implementation, specifying the standard WAV format.
    return output_wav_common_initialize(context, SF_FORMAT_WAV);
}

/**
 * @brief Populates the summary info for a standard WAV output.
 */
static void output_wav_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context; // Unused in this simple implementation
    utility_add_summary_item(info, "Output Type", "WAV");
}

static const struct argparse_option output_wav_cli_options[] = {
    OPT_GROUP("WAV Output (wav)"),
    OPT_GROUP("    (No module-specific options)"),
};

const struct argparse_option* output_wav_get_cli_options(int* count) {
    *count = sizeof(output_wav_cli_options) / sizeof(output_wav_cli_options[0]);
    return output_wav_cli_options;
}

/**
 * @brief The v-table (virtual function table) for the WAV output module.
 *
 * This struct wires up the public interface to the functions in this file
 * and the shared functions from the common WAV module.
 */
static OutputModuleInterface s_output_wav_api = {
    .validate_options = output_wav_common_validate_options, // Use common validation
    .get_cli_options  = output_wav_get_cli_options,
    .initialize       = output_wav_initialize,       // Use our specific initializer
    .reset = NULL,
    .flush = NULL,
    .write_chunk = output_wav_common_write_chunk,           // Use common direct-write function
    .cleanup = output_wav_common_cleanup,                   // Use common finalizer
    .get_summary_info = output_wav_get_summary_info, // Use our specific summary function
};

/**
 * @brief Public getter for the WAV output module's interface.
 */
OutputModuleInterface* output_wav_get_module_api(void) {
    return &s_output_wav_api;
}
