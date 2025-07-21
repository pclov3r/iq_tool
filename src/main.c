#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "cli.h"
#include "resample.h"
#include "utils.h"

/**
 * @brief Initializes a resource struct to a clean, known state.
 * @param resources Pointer to the AppResources struct to initialize.
 */
static void initialize_resource_struct(AppResources *resources) {
    memset(resources, 0, sizeof(AppResources));
    resources->final_output_size_bytes = -1LL;
#ifdef _WIN32
    resources->h_outfile = INVALID_HANDLE_VALUE;
#endif
    resources->resampler = NULL;
    resources->shifter_nco = NULL;
    resources->infile = NULL;
    resources->outfile = NULL;
    resources->work_item_pool = NULL;
    resources->raw_input_pool = NULL;
    resources->complex_scaled_pool = NULL;
    resources->complex_shifted_pool = NULL;
    resources->complex_resampled_pool = NULL;
    resources->output_pool = NULL;
    resources->free_pool_q = NULL;
    resources->input_q = NULL;
    resources->output_q = NULL;
}

/**
 * @brief Prints a final summary of the processing results to stderr.
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources.
 * @param success True if the operation completed successfully, false otherwise.
 */
static void print_final_summary(const AppConfig *config, AppResources *resources, bool success) {
    // No summary is printed when outputting to stdout
    if (config->output_to_stdout) {
        if (success) {
            fflush(stdout); // Ensure stdout buffer is flushed on success
        }
        return;
    }

    const int final_summary_label_width = 32;
    char output_size_str_buf[40];
    format_file_size(resources->final_output_size_bytes, output_size_str_buf, sizeof(output_size_str_buf));

    if (success) {
        double final_percentage = 0.0;
        if (resources->sfinfo.frames > 0) {
            final_percentage = ((double)resources->total_frames_read / (double)resources->sfinfo.frames) * 100.0;
            if (final_percentage > 100.0) final_percentage = 100.0; // Clamp
        }

        // Overwrite the final progress line with a "Done" message
        fprintf(stderr, "\rProcessed %llu / %llu input frames (%.1f%%)... Done.\n",
                resources->total_frames_read,
                (unsigned long long)resources->sfinfo.frames,
                final_percentage);

        fprintf(stderr, "\n"); // Blank line before totals

        unsigned long long total_output_samples = resources->total_output_frames * 2;
        fprintf(stderr, "%-*s %llu\n", final_summary_label_width, "Total output frames generated:", resources->total_output_frames);
        fprintf(stderr, "%-*s %llu\n", final_summary_label_width, "Total output samples generated:", total_output_samples);
        fprintf(stderr, "%-*s %s\n", final_summary_label_width, "Output File Size:", output_size_str_buf);

        fprintf(stderr, "\nResample operation completed successfully.\n");
    } else {
        // On failure, the specific error was already printed by the thread that failed.
        // This just provides a final status.
        fprintf(stderr, "\nFATAL: Processing stopped due to an error after %llu input frames.\n", resources->total_frames_read);
        fprintf(stderr, "%-*s %s (possibly incomplete)\n", final_summary_label_width, "Output File Size:", output_size_str_buf);
        fprintf(stderr, "\n");
    }
}

/**
 * @brief Main entry point for the nrsc5 resample tool.
 *
 * Orchestrates the program flow:
 * 1. Parses command line arguments.
 * 2. Initializes resources (files, buffers, pools, queues, DSP objects, metadata).
 * 3. Runs the processing threads (Reader, Processor, Writer).
 * 4. Cleans up all allocated resources.
 * 5. Prints a final summary.
 */
int main(int argc, char *argv[]) {
    AppConfig config;
    AppResources resources;
    int exit_status = EXIT_FAILURE; // Assume failure initially
    bool init_ok = false;
    bool processing_ok = false;

    // Initialize structs to a known state (zeroes/NULLs/etc.)
    // This is important for the cleanup logic, which safely handles NULL pointers.
    memset(&config, 0, sizeof(AppConfig));
    initialize_resource_struct(&resources);

    // 1. Parse Command Line Arguments
    if (!parse_arguments(argc, argv, &config)) {
        print_usage(argv[0]);
        fflush(stderr);
        return EXIT_FAILURE;
    }

    // 2. Initialize All Application Resources
    // This function handles file validation, buffer allocation, DSP setup, etc.
    init_ok = initialize_resources(&config, &resources);
    if (!init_ok) {
        // Error messages are printed by the initialization sub-functions.
        // The cleanup function is designed to be safe even on partial initialization.
        fprintf(stderr, "Resource initialization failed.\n");
        cleanup_resources(&config, &resources);
        fflush(stderr);
        return EXIT_FAILURE;
    }

    // 3. Run the Multi-Threaded Processing Pipeline
    // This function creates, runs, and joins the threads. It returns false if an
    // error occurred in any thread.
    processing_ok = run_processing_threads(&config, &resources);

    // 4. Clean Up All Allocated Resources
    // This must be called after threads are joined to avoid race conditions.
    // It also determines the final output file size before closing the file.
    cleanup_resources(&config, &resources);

    // 5. Print Final Summary and Determine Exit Status
    if (processing_ok) {
        exit_status = EXIT_SUCCESS;
    } else {
        exit_status = EXIT_FAILURE;
    }
    print_final_summary(&config, &resources, processing_ok);

    fflush(stderr);
    return exit_status;
}
