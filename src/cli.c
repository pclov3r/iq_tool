#include "cli.h"
#include "types.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

// Use getopt_long for both short and long options
#ifdef _WIN32
#include <getopt.h> // Assumes a working getopt is available (e.g., from vcpkg)
#else
#include <unistd.h> // Standard POSIX getopt
#include <getopt.h>
#endif

/**
 * @brief Prints command usage information to stderr.
 */
void print_usage(const char *prog_name) {
    const int option_width = 29; // Width for the option column

    fprintf(stderr, "Usage: %s --input <file> {--file <path> | --stdout} [options]\n\n", prog_name);
    fprintf(stderr, "Description:\n");
    fprintf(stderr, "  Resamples an I/Q WAV file to a format compatible with the nrsc5 HD Radio decoder.\n\n");

    fprintf(stderr, "Required Arguments:\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "-i, --input <file>", "I/Q input file (8-bit or 16-bit WAV).\n");

    fprintf(stderr, "Output Destination (Required, choose one):\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "-f, --file <file>", "Output to a file.");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "-o, --stdout", "Output binary data to stdout for piping to another program (e.g., nrsc5).");

    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "-m, --mode <name>", "Output mode. (Optional, Default: cu8).");
    fprintf(stderr, "  %-*s %s\n", option_width, "", "  cu8:      Universal 8-bit for AM/FM HD Radio decoding (1488375 Hz).");
    fprintf(stderr, "  %-*s %s\n", option_width, "", "  cs16_fm:  16-bit complex for FM HD Radio decoding (744187.5 Hz).");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "", "  cs16_am:  16-bit complex for AM HD Radio decoding (46511.71875 Hz).");

    fprintf(stderr, "  %-*s %s\n", option_width, "-c, --target-freq <hz>", "Shift signal to a new target center frequency (e.g., 97.3e6).");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "", "(Recommended for SDR captures with frequency metadata).");

    fprintf(stderr, "  %-*s %s\n", option_width, "-F, --shift-freq <hz>", "Apply a direct frequency shift in Hz.");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "", "(Use if input lacks metadata or for manual correction).");

    fprintf(stderr, "  %-*s %s\n", option_width, "-x, --shift-after", "Apply frequency shift AFTER resampling (default is before).");
    fprintf(stderr, "  %-*s %s\n", option_width, "", "(A workaround for narrow I/Q recordings where only a single");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "", " HD sideband is present).");

    fprintf(stderr, "  %-*s %s\n", option_width, "-s, --scale <value>", "Scaling factor for input samples (Default: 0.02 for cu8, 0.5 for cs16).");
}


/**
 * @brief Parses command line arguments using getopt_long.
 */
bool parse_arguments(int argc, char *argv[], AppConfig *config) {
    // Initialize config struct to default values
    memset(config, 0, sizeof(AppConfig));
    config->mode_name = NULL; // Will be set to default later if not provided
    config->scale_value = 0.0f;
    config->scale_provided = false;
    config->output_to_stdout = false;
    config->freq_shift_hz = 0.0;
    config->freq_shift_requested = false;
    config->center_frequency_target_hz = 0.0;
    config->set_center_frequency_target_hz = false;
    config->shift_after_resample = false;
    config->mode = MODE_INVALID;
    config->target_rate = 0.0;

    // --- Argument Parsing Setup ---
    int opt;
    int long_index = 0;
    bool output_to_file_flag = false;

    // Short options string for getopt_long
    const char* short_opts = "i:s:m:of:F:c:x";

    // Long options mapping
    static const struct option long_options[] = {
        {"input",       required_argument, 0, 'i'},
        {"scale",       required_argument, 0, 's'},
        {"mode",        required_argument, 0, 'm'},
        {"stdout",      no_argument,       0, 'o'},
        {"file",        required_argument, 0, 'f'},
        {"shift-freq",  required_argument, 0, 'F'},
        {"target-freq", required_argument, 0, 'c'},
        {"shift-after", no_argument,       0, 'x'},
        {0, 0, 0, 0} // Terminator
    };

    // --- Argument Parsing Loop ---
    while ((opt = getopt_long(argc, argv, short_opts, long_options, &long_index)) != -1) {
         switch (opt) {
            case 'i': config->input_filename_arg = optarg; break;
            case 's':
                {
                    char *endptr;
                    errno = 0;
                    config->scale_value = strtof(optarg, &endptr);
                    if (errno != 0 || *endptr != '\0' || !isfinite(config->scale_value) || config->scale_value <= 0) {
                        fprintf(stderr, "Error: Invalid scaling factor '%s'. Must be a positive finite number.\n", optarg);
                        return false;
                    }
                    config->scale_provided = true;
                }
                break;
             case 'F':
                {
                    char *endptr;
                    errno = 0;
                    config->freq_shift_hz = strtod(optarg, &endptr);
                    if (errno != 0 || *endptr != '\0' || !isfinite(config->freq_shift_hz)) {
                        fprintf(stderr, "Error: Invalid frequency shift value '%s'. Must be a valid finite number.\n", optarg);
                        return false;
                    }
                    config->freq_shift_requested = true;
                }
                break;
            case 'c':
                {
                    char *endptr;
                    errno = 0;
                    config->center_frequency_target_hz = strtod(optarg, &endptr);
                    if (errno != 0 || *endptr != '\0' || !isfinite(config->center_frequency_target_hz)) {
                        fprintf(stderr, "Error: Invalid target frequency value '%s'. Must be a valid finite number.\n", optarg);
                        return false;
                    }
                    config->set_center_frequency_target_hz = true;
                }
                break;
            case 'm': config->mode_name = optarg; break;
            case 'o': config->output_to_stdout = true; break;
            case 'f': output_to_file_flag = true; config->output_filename_arg = optarg; break;
            case 'x': config->shift_after_resample = true; break;
            case '?':
                // Error message is printed by getopt_long, just add a newline for spacing
                fprintf(stderr, "\n");
                return false;
            default:
                // Should not be reached
                fprintf(stderr, "Internal Error: Unexpected option processing error.\n");
                return false;
        }
    }

    // --- Argument Validation (Post-Parsing) ---

    // Check for required arguments
    if (!config->input_filename_arg) {
        fprintf(stderr, "Error: Missing required argument --input <file>.\n");
        return false;
    }
    if (!config->output_to_stdout && !output_to_file_flag) {
        fprintf(stderr, "Error: Must specify an output destination: --stdout or --file <file>.\n");
        return false;
    }

    // Check for mutually exclusive arguments
    if (config->output_to_stdout && output_to_file_flag) {
        fprintf(stderr, "Error: Options --stdout and --file <file> are mutually exclusive.\n");
        return false;
    }
    if (config->freq_shift_requested && config->set_center_frequency_target_hz) {
        fprintf(stderr, "Error: Options --shift-freq and --target-freq are mutually exclusive.\n");
        return false;
    }

    // Check for dependent arguments
    if (config->shift_after_resample && !config->freq_shift_requested && !config->set_center_frequency_target_hz) {
        fprintf(stderr, "Error: Option --shift-after specified, but no frequency shift was requested.\n");
        return false;
    }

    // Check for unexpected non-option arguments
    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected non-option arguments found:\n");
        for (int i = optind; i < argc; i++) { fprintf(stderr, "  %s\n", argv[i]); }
        return false;
    }

    // --- Set Mode, Target Rate, and Default Scale (if needed) ---

    // If mode was not provided by the user, set it to "cu8" to trigger the default logic.
    if (!config->mode_name) {
        config->mode_name = "cu8";
    }

    if (strcmp(config->mode_name, "cu8") == 0) {
        config->mode = MODE_CU8;
        config->target_rate = TARGET_RATE_CU8;
        if (!config->scale_provided) {
            config->scale_value = DEFAULT_SCALE_FACTOR_CU8;
        }
    } else if (strcmp(config->mode_name, "cs16_fm") == 0) {
        config->mode = MODE_CS16_FM;
        config->target_rate = TARGET_RATE_CS16_FM;
        if (!config->scale_provided) {
            config->scale_value = DEFAULT_SCALE_FACTOR_CS16;
        }
    } else if (strcmp(config->mode_name, "cs16_am") == 0) {
        config->mode = MODE_CS16_AM;
        config->target_rate = TARGET_RATE_CS16_AM;
        if (!config->scale_provided) {
            config->scale_value = DEFAULT_SCALE_FACTOR_CS16;
        }
    } else {
        fprintf(stderr, "Error: Invalid output mode '%s'. Valid modes are: cu8, cs16_fm, cs16_am.\n", config->mode_name);
        return false;
    }

    return true; // Success
}
