#include "cli.h"
#include "types.h"
#include "config.h"
#include "log.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

#include "argparse.h"
#include "input_manager.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

extern AppConfig g_config;

// --- Forward Declarations ---
static bool validate_output_destination(AppConfig *config);
static bool validate_output_type_and_sample_format(AppConfig *config);
static bool validate_sdr_general_options(AppConfig *config);
static bool validate_iq_correction_options(AppConfig *config);
static bool validate_and_process_args(AppConfig *config, int non_opt_argc, const char** non_opt_argv);
static bool resolve_frequency_shift_options(AppConfig *config);


void print_usage(const char *prog_name) {
    const char* help_argv[] = { prog_name, "--help" };
    parse_arguments(2, (char**)help_argv, &g_config);
}


bool parse_arguments(int argc, char *argv[], AppConfig *config) {
    // Increase max options to accommodate dynamic presets
    #define MAX_STATIC_OPTIONS 128
    #define MAX_TOTAL_OPTIONS (MAX_STATIC_OPTIONS + MAX_PRESETS)
    struct argparse_option all_options[MAX_TOTAL_OPTIONS];
    int total_opts = 0;

    static const struct argparse_option generic_options[] = {
        OPT_GROUP("Required Input & Output"),
        OPT_STRING('i', "input", &g_config.input_type_str, "Specifies the input type {wav|raw-file|rtlsdr|sdrplay|hackrf|bladerf}", NULL, 0, 0),
        OPT_STRING('f', "file", &g_config.output_filename_arg, "Output to a file.", NULL, 0, 0),
        OPT_BOOLEAN('o', "stdout", &g_config.output_to_stdout, "Output binary data for piping to another program.", NULL, 0, 0),

        OPT_GROUP("Output Options"),
        OPT_STRING(0, "output-container", &g_config.output_type_name, "Specifies the output file container format {raw|wav|wav-rf64}", NULL, 0, 0),
        OPT_STRING(0, "output-sample-format", &g_config.sample_type_name, "Sample format for output data {cs8|cu8|cs16|...}", NULL, 0, 0),

        OPT_GROUP("Processing Options"),
        OPT_FLOAT(0, "output-rate", &g_config.user_defined_target_rate_arg, "Output sample rate in Hz. (Required if no preset is used)", NULL, 0, 0),
        OPT_FLOAT(0, "gain", &g_config.gain, "Apply a linear gain multiplier to the samples (Default: 1.0)", NULL, 0, 0),
        OPT_FLOAT(0, "freq-shift", &g_config.freq_shift_hz_arg, "Apply a direct frequency shift in Hz (e.g., -100e3)", NULL, 0, 0),
        OPT_BOOLEAN(0, "shift-after-resample", &g_config.shift_after_resample, "Apply frequency shift AFTER resampling (default is before)", NULL, 0, 0),
        OPT_BOOLEAN(0, "no-resample", &g_config.no_resample, "Process at native input rate. Bypasses the resampler but applies all other DSP.", NULL, 0, 0),
        OPT_BOOLEAN(0, "raw-passthrough", &g_config.raw_passthrough, "Bypass all processing. Copies raw input bytes directly to output.", NULL, 0, 0),
        OPT_BOOLEAN(0, "iq-correction", &g_config.iq_correction.enable, "(Optional) Enable automatic I/Q imbalance correction.", NULL, 0, 0),
        OPT_BOOLEAN(0, "dc-block", &g_config.dc_block.enable, "(Optional) Enable DC offset removal (high-pass filter).", NULL, 0, 0),
        OPT_STRING(0, "preset", &g_config.preset_name, "Use a preset for a common target.", NULL, 0, 0),
    };

    #if defined(ANY_SDR_SUPPORT_ENABLED)
    static const struct argparse_option sdr_general_options[] = {
        OPT_GROUP("SDR General Options"),
        OPT_FLOAT(0, "rf-freq", &g_config.sdr.sdr_rf_freq_hz_arg, "(Required for SDR) Tuner center frequency in Hz (e.g., 97.3e6)", NULL, 0, 0),
        OPT_BOOLEAN(0, "bias-t", &g_config.sdr.bias_t_enable, "(Optional) Enable Bias-T power.", NULL, 0, 0),
    };
    #endif

    // --- Define placeholder and final options ---
    static struct argparse_option final_options[] = {
        OPT_GROUP("Help"),
        OPT_BOOLEAN('h', "help", NULL, "show this help message and exit", argparse_help_cb, 0, OPT_NONEG),
        OPT_END(),
    };

    #define SAFE_MEMCPY(dest, src, n) \
        do { \
            if ((size_t)(total_opts + (n)) > MAX_TOTAL_OPTIONS) { \
                log_fatal("Internal error: Exceeded maximum number of CLI options."); \
                return false; \
            } \
            memcpy(dest, src, (n) * sizeof(struct argparse_option)); \
            total_opts += (n); \
        } while (0)

    // --- Build the master options list in the desired order ---
    SAFE_MEMCPY(&all_options[total_opts], generic_options, sizeof(generic_options) / sizeof(generic_options[0]));
    #if defined(ANY_SDR_SUPPORT_ENABLED)
    SAFE_MEMCPY(&all_options[total_opts], sdr_general_options, sizeof(sdr_general_options) / sizeof(sdr_general_options[0]));
    #endif

    int num_modules = 0;
    const InputModule* modules = get_all_input_modules(&num_modules);
    for (int i = 0; i < num_modules; ++i) {
        if (modules[i].get_cli_options) {
            int count = 0;
            const struct argparse_option* opts = modules[i].get_cli_options(&count);
            if (opts && count > 0) {
                SAFE_MEMCPY(&all_options[total_opts], opts, count);
            }
        }
    }

    // --- Dynamically build and add preset options ---
    if (config->num_presets > 0) {
        struct argparse_option preset_header[] = { OPT_GROUP("Available Presets:") };
        SAFE_MEMCPY(&all_options[total_opts], preset_header, 1);

        // Create a temporary array on the stack for the preset options
        struct argparse_option preset_opts[MAX_PRESETS];
        int presets_to_add = (config->num_presets > MAX_PRESETS) ? MAX_PRESETS : config->num_presets;

        for (int i = 0; i < presets_to_add; i++) {
            preset_opts[i] = (struct argparse_option){
                .type = ARGPARSE_OPT_BOOLEAN, // A simple type for display purposes
                .long_name = config->presets[i].name,
                .help = config->presets[i].description,
                .flags = OPT_LONG_NOPREFIX, // Use our new flag
            };
        }
        SAFE_MEMCPY(&all_options[total_opts], preset_opts, presets_to_add);
    }

    SAFE_MEMCPY(&all_options[total_opts], final_options, sizeof(final_options) / sizeof(final_options[0]));

    struct argparse argparse;
    const char *const usages[] = {
        "iq_resample_tool -i <type> [input_file] [options]",
        NULL,
    };
    argparse_init(&argparse, all_options, usages, 0);
    argparse_describe(&argparse, "\nResamples an I/Q file or a stream from an SDR device to a specified format and sample rate.", NULL);
    
    int non_opt_argc = argparse_parse(&argparse, argc, (const char **)argv);

    if (!validate_and_process_args(config, non_opt_argc, argparse.out)) {
        return false;
    }

    return true;
}

// --- NEW: The Final, Generic Resolver Function ---
// This function contains ALL logic related to frequency shifting.
// It is the single source of truth.
static bool resolve_frequency_shift_options(AppConfig *config) {
    // --- Part 1: Populate the request struct from generic arguments ---
    if (config->freq_shift_hz_arg != 0.0f) {
        // Check if a module (like WAV) has already made a request.
        if (config->frequency_shift_request.type != FREQUENCY_SHIFT_REQUEST_NONE) {
            log_fatal("Conflicting frequency shift options provided. Cannot use --freq-shift and --wav-center-target-frequency at the same time.");
            return false;
        }
        config->frequency_shift_request.type = FREQUENCY_SHIFT_REQUEST_MANUAL;
        config->frequency_shift_request.value = (double)config->freq_shift_hz_arg;
    }

    // --- Part 2: Resolve the single, verified request into the final DSP config ---
    switch (config->frequency_shift_request.type) {
        case FREQUENCY_SHIFT_REQUEST_NONE:
            config->freq_shift_requested = false;
            break;

        case FREQUENCY_SHIFT_REQUEST_MANUAL:
            config->freq_shift_requested = true;
            config->freq_shift_hz = config->frequency_shift_request.value;
            break;

        case FREQUENCY_SHIFT_REQUEST_METADATA_CALC_TARGET:
            config->freq_shift_requested = true;
            config->set_center_frequency_target_hz = true;
            config->center_frequency_target_hz = config->frequency_shift_request.value;
            break;
    }

    // --- Part 3: Final validation of related options ---
    if (config->shift_after_resample && !config->freq_shift_requested) {
        log_fatal("Option --shift-after-resample was used, but no frequency shift was requested.");
        return false;
    }

    return true;
}

// --- MODIFIED: The main validation flow ---
static bool validate_and_process_args(AppConfig *config, int non_opt_argc, const char** non_opt_argv) {
    if (!config->input_type_str) {
        fprintf(stderr, "error: missing required argument --input <type>\n");
        return false;
    }

    InputSourceOps* selected_ops = get_input_ops_by_name(config->input_type_str);
    if (!selected_ops) {
        log_fatal("Invalid input type '%s'.", config->input_type_str);
        return false;
    }

    bool is_file_input = (strcasecmp(config->input_type_str, "wav") == 0 ||
                          strcasecmp(config->input_type_str, "raw-file") == 0);

    if (is_file_input) {
        if (non_opt_argc == 0) {
            log_fatal("Missing <file_path> argument for '--input %s'.", config->input_type_str);
            return false;
        }
        if (non_opt_argc > 1) {
            log_fatal("Unexpected non-option arguments found. Only one input file path is allowed.");
            return false;
        }
        config->input_filename_arg = (char*)non_opt_argv[0];
    } else {
        if (non_opt_argc > 0) {
            log_fatal("Unexpected non-option argument '%s' found for non-file input.", non_opt_argv[0]);
            return false;
        }
    }

    // Initialize the request type to NONE before any validation happens.
    config->frequency_shift_request.type = FREQUENCY_SHIFT_REQUEST_NONE;

    // Run the module-specific validation FIRST to populate the request struct.
    if (selected_ops->validate_options) {
        if (!selected_ops->validate_options(config)) {
            return false;
        }
    }

    // Run other generic validation functions.
    if (!validate_output_destination(config)) return false;
    if (!validate_output_type_and_sample_format(config)) return false;
    if (!validate_sdr_general_options(config)) return false;

    // Call our new, clean resolver for frequency shifts.
    if (!resolve_frequency_shift_options(config)) return false;

    // Validate the remaining processing options that are NOT related to frequency shifting.
    if (config->user_rate_provided && config->preset_name) {
        log_fatal("Option --output-rate cannot be used with --preset.");
        return false;
    }
    if (config->no_resample) {
        if (config->user_rate_provided) {
            log_fatal("Option --no-resample cannot be used with --output-rate.");
            return false;
        }
        if (config->preset_name) {
            log_fatal("Option --no-resample cannot be used with --preset.");
            return false;
        }
    }
    if (config->raw_passthrough) {
        if (!config->no_resample) {
            log_warn("Option --raw-passthrough implies --no-resample. Forcing resampler off.");
            config->no_resample = true;
        }
        if (config->freq_shift_requested) {
            log_fatal("Option --raw-passthrough cannot be used with frequency shifting options.");
            return false;
        }
        if (config->iq_correction.enable) {
            log_fatal("Option --raw-passthrough cannot be used with --iq-correction.");
            return false;
        }
        if (config->dc_block.enable) {
            log_fatal("Option --raw-passthrough cannot be used with --dc-block.");
            return false;
        }
    }
    if (config->target_rate <= 0 && !config->no_resample) {
        log_fatal("Missing required argument: you must specify an --output-rate or use a preset.");
        return false;
    }

    if (!validate_iq_correction_options(config)) return false;

    return true;
}

static bool validate_output_destination(AppConfig *config) {
    if (config->output_to_stdout && config->output_filename_arg) {
        log_fatal("Options --stdout and --file <file> are mutually exclusive.");
        return false;
    }
    if (!config->output_to_stdout && !config->output_filename_arg) {
        log_fatal("Must specify an output destination: --stdout or --file <file>.");
        return false;
    }
    return true;
}

static bool validate_output_type_and_sample_format(AppConfig *config) {
    if (config->preset_name) {
        bool preset_found = false;
        for (int i = 0; i < config->num_presets; i++) {
            if (strcasecmp(config->preset_name, config->presets[i].name) == 0) {
                const PresetDefinition* p = &config->presets[i];
                config->target_rate = p->target_rate;
                if (!config->sample_type_name) {
                    config->sample_type_name = p->sample_format_name;
                }
                if (!config->output_type_name) {
                    config->output_type = p->output_type;
                    config->output_type_provided = true;
                }
                if (p->gain_provided && config->gain == 1.0f) {
                    config->gain = p->gain;
                }
                if (p->dc_block_provided && !config->dc_block.enable) {
                    config->dc_block.enable = p->dc_block_enable;
                }
                if (p->iq_correction_provided && !config->iq_correction.enable) {
                    config->iq_correction.enable = p->iq_correction_enable;
                }
                preset_found = true;
                break;
            }
        }
        if (!preset_found) {
            log_fatal("Unknown preset '%s'. Check '%s' or --help for available presets.", config->preset_name, PRESETS_FILENAME);
            return false;
        }
    }

    if (config->output_type_name) {
        config->output_type_provided = true;
        if (strcasecmp(config->output_type_name, "raw") == 0) config->output_type = OUTPUT_TYPE_RAW;
        else if (strcasecmp(config->output_type_name, "wav") == 0) config->output_type = OUTPUT_TYPE_WAV;
        else if (strcasecmp(config->output_type_name, "wav-rf64") == 0) config->output_type = OUTPUT_TYPE_WAV_RF64;
        else {
            log_fatal("Invalid output type '%s'. Must be 'raw', 'wav', or 'wav-rf64'.", config->output_type_name);
            return false;
        }
    } else if (!config->output_type_provided) {
        config->output_type = config->output_to_stdout ? OUTPUT_TYPE_RAW : OUTPUT_TYPE_WAV_RF64;
    }

    if (config->user_defined_target_rate_arg > 0.0f) {
        config->target_rate = (double)config->user_defined_target_rate_arg;
        config->user_rate_provided = true;
    }

    if (!config->sample_type_name) {
        if (config->output_filename_arg && !config->output_to_stdout) {
            config->sample_type_name = "cs16";
        } else {
            log_fatal("Missing required argument: you must specify an --output-sample-format or use a preset.");
            return false;
        }
    }

    config->output_format = utils_get_format_from_string(config->sample_type_name);
    if (config->output_format == FORMAT_UNKNOWN) {
        log_fatal("Invalid sample format '%s'. See --help for valid formats.", config->sample_type_name);
        return false;
    }

    if (config->output_to_stdout && (config->output_type == OUTPUT_TYPE_WAV || config->output_type == OUTPUT_TYPE_WAV_RF64)) {
        log_fatal("Invalid option: WAV/RF64 container format cannot be used with --stdout.");
        return false;
    }

    if (config->output_type == OUTPUT_TYPE_WAV || config->output_type == OUTPUT_TYPE_WAV_RF64) {
        if (config->output_format != CS16 && config->output_format != CU8) {
            log_fatal("Invalid sample format '%s' for WAV container. Only 'cs16' and 'cu8' are supported for WAV output.", config->sample_type_name);
            return false;
        }
    }

    return true;
}

static bool validate_sdr_general_options(AppConfig *config) {
    #if defined(ANY_SDR_SUPPORT_ENABLED)
    bool is_sdr = is_sdr_input(config->input_type_str);

    if (config->sdr.sdr_rf_freq_hz_arg > 0.0f) {
        config->sdr.rf_freq_hz = (double)config->sdr.sdr_rf_freq_hz_arg;
        config->sdr.rf_freq_provided = true;
    }

    if (is_sdr && !config->sdr.rf_freq_provided) {
        log_fatal("Option '--rf-freq' is required for SDR inputs.");
        return false;
    }
    if (!is_sdr) {
        if (config->sdr.rf_freq_provided) {
            log_fatal("Option '--rf-freq' is only valid for SDR inputs.");
            return false;
        }
        if (config->sdr.bias_t_enable) {
            log_fatal("Option '--bias-t' is only valid for SDR inputs.");
            return false;
        }
    }
    #else
    (void)config;
    #endif
    return true;
}

static bool validate_iq_correction_options(AppConfig *config) {
    if (config->iq_correction.enable) {
        if (!config->dc_block.enable) {
            log_fatal("Option --iq-correction requires --dc-block to be enabled for optimal performance and stability.");
            return false;
        }
    }
    return true;
}
