/**
 * @file output_wav_rf64.c
 * @brief Implements the WAV/RF64 file output module for large file support.
 *
 * This file is a lightweight wrapper around the common WAV writing logic.
 * Its only job is to specify the SF_FORMAT_RF64 flag during initialization
 * and provide the correct summary information.
 */

#include "output_wav_rf64.h"
#include "output_wav_common.h" // Include the shared implementation
#include "utils.h"             // For add_summary_item
#include <sndfile.h>           // For the SF_FORMAT_RF64 constant

/**
 * @brief Initializes the WAV/RF64 writer by calling the common initializer.
 *
 * This function's sole responsibility is to pass the specific format flag
 * for RF64 files to the shared initialization logic.
 */
static bool wav_rf64_initialize(ModuleContext* ctx) {
    // Call the common implementation, specifying the RF64 format.
    return wav_common_initialize(ctx, SF_FORMAT_RF64);
}

/**
 * @brief Populates the summary info for a WAV/RF64 output.
 */
static void wav_rf64_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx; // Unused in this simple implementation
    add_summary_item(info, "Output Type", "WAV (RF64)");
}

/**
 * @brief The v-table for the WAV/RF64 output module.
 *
 * This struct wires up the public interface to the functions in this file
 * and the shared functions from the common WAV module.
 */
static OutputModuleInterface wav_rf64_module_api = {
    .validate_options = wav_common_validate_options,  // Use common validation
    .get_cli_options  = NULL,                         // No specific CLI options
    .initialize       = wav_rf64_initialize,          // Use our specific initializer
    .run_writer       = wav_common_run_writer,        // Use common writer thread loop
    .write_chunk      = wav_common_write_chunk,       // Use common direct-write function
    .finalize_output  = wav_common_finalize_output,   // Use common finalizer
    .get_summary_info = wav_rf64_get_summary_info,    // Use our specific summary function
};

/**
 * @brief Public getter for the WAV/RF64 output module's interface.
 */
OutputModuleInterface* get_wav_rf64_output_module_api(void) {
    return &wav_rf64_module_api;
}
