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
#include "utils.h"             // For add_summary_item
#include <sndfile.h>           // For the SF_FORMAT_WAV constant

/**
 * @brief Initializes the WAV writer by calling the common initializer.
 *
 * This function's sole responsibility is to pass the specific format flag
 * for standard WAV files to the shared initialization logic.
 */
static bool wav_output_initialize(ModuleContext* ctx) {
    // Call the common implementation, specifying the standard WAV format.
    return wav_common_initialize(ctx, SF_FORMAT_WAV);
}

/**
 * @brief Populates the summary info for a standard WAV output.
 */
static void wav_output_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx; // Unused in this simple implementation
    add_summary_item(info, "Output Type", "WAV (Standard)");
}

static const struct argparse_option wav_output_cli_options[] = {
    OPT_GROUP("WAV Output (wav)"),
    OPT_GROUP("    (No module-specific options)"),
};

const struct argparse_option* wav_output_get_cli_options(int* count) {
    *count = sizeof(wav_output_cli_options) / sizeof(wav_output_cli_options[0]);
    return wav_output_cli_options;
}

/**
 * @brief The v-table (virtual function table) for the WAV output module.
 *
 * This struct wires up the public interface to the functions in this file
 * and the shared functions from the common WAV module.
 */
static OutputModuleInterface s_wav_output_api = {
    .validate_options = wav_common_validate_options, // Use common validation
    .get_cli_options  = wav_output_get_cli_options,
    .initialize       = wav_output_initialize,       // Use our specific initializer
    .reset = NULL,
    .flush = NULL,
    .write_chunk = wav_common_write_chunk,           // Use common direct-write function
    .cleanup = wav_common_cleanup,                   // Use common finalizer
    .get_summary_info = wav_output_get_summary_info, // Use our specific summary function
};

/**
 * @brief Public getter for the WAV output module's interface.
 */
OutputModuleInterface* output_wav_get_module_api(void) {
    return &s_wav_output_api;
}
