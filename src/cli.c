// cli.c

#include "cli.h"
#include "constants.h"
#include "app_context.h"      // Provides AppConfig, MemoryArena
#include "config.h"           // Provides validation function prototypes
#include "log.h"
#include "utils.h"
#include "argparse.h"
#include "module_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

extern AppConfig g_config;

// MODIFIED: Store original command-line arguments for improved error reporting.
static int g_original_argc = 0;
static const char** g_original_argv = NULL;

// MODIFIED: Moved these definitions to file scope to be accessible by all functions.
#define MAX_STATIC_OPTIONS 128
#define MAX_TOTAL_OPTIONS (MAX_STATIC_OPTIONS + MAX_PRESETS)

// --- Forward Declarations ---
static bool validate_and_process_args(AppConfig *config, int non_opt_argc, const char** non_opt_argv, MemoryArena* arena);
static int version_cb(struct argparse *self, const struct argparse_option *option);
static int build_cli_options(struct argparse_option* options_buffer, int max_options, AppConfig* config, MemoryArena* arena, const char* active_input_type);


void print_usage(const char *prog_name, AppConfig *config, MemoryArena* arena) {
    (void)prog_name; // MODIFIED: Mark as unused to silence the warning.
    struct argparse argparse;
    struct argparse_option all_options[MAX_TOTAL_OPTIONS];
    const char *const usages[] = {
        "iq_tool -i <type> [input_file] [options]",
        NULL,
    };

    // Build the full options list to generate complete help text.
    // Pass NULL for active_input_type to ensure all module options are included in the help text.
    build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena, NULL);

    argparse_init(&argparse, all_options, usages, 0);
    argparse_describe(&argparse, "\nResamples an I/Q file or a stream from an SDR device to a specified format and sample rate.", NULL);
    argparse_usage(&argparse);
}

static int version_cb(struct argparse *self, const struct argparse_option *option) {
    (void)self;
    (void)option;

#ifdef GIT_HASH
    fprintf(stdout, "%s version %s\n", APP_NAME, GIT_HASH);
#else
    fprintf(stdout, "%s version unknown\n", APP_NAME);
#endif

    exit(EXIT_SUCCESS);
}

static int build_cli_options(struct argparse_option* options_buffer, int max_options, AppConfig* config, MemoryArena* arena, const char* active_input_type) {
    int total_opts = 0;

    static const struct argparse_option generic_options[] = {
        OPT_GROUP("Required Input & Output"),
        OPT_STRING('i', "input", &g_config.input_type_str, "Specifies the input type {wav|raw-file|rtlsdr|sdrplay|hackrf|bladerf|spyserver-client}", NULL, 0, 0),
        OPT_STRING('f', "file", &g_config.output_filename_arg, "Output to a file.", NULL, 0, 0),
        OPT_BOOLEAN('o', "stdout", &g_config.output_to_stdout, "Output binary data for piping to another program.", NULL, 0, 0),
        OPT_GROUP("Output Options"),
        OPT_STRING(0, "output-container", &g_config.output_type_name, "Specifies the output file container format {raw|wav|wav-rf64}", NULL, 0, 0),
        OPT_STRING(0, "output-sample-format", &g_config.sample_type_name, "Sample format for output data {cs8|cu8|cs16|...}", NULL, 0, 0),
        OPT_GROUP("Processing Options"),
        OPT_FLOAT(0, "output-rate", &g_config.user_defined_target_rate_arg, "Output sample rate in Hz. (Required if no preset or --no-resample is used)", NULL, 0, 0),
        OPT_FLOAT(0, "gain-multiplier", &g_config.gain, "Apply a linear gain multiplier to the samples", NULL, 0, 0),
        OPT_FLOAT(0, "freq-shift", &g_config.freq_shift_hz_arg, "Apply a direct frequency shift in Hz (e.g., -100e3)", NULL, 0, 0),
        OPT_BOOLEAN(0, "shift-after-resample", &g_config.shift_after_resample, "Apply frequency shift AFTER resampling (default is before)", NULL, 0, 0),
        OPT_BOOLEAN(0, "no-resample", &g_config.no_resample, "Process at native input rate. Bypasses the resampler but applies all other DSP.", NULL, 0, 0),
        OPT_BOOLEAN(0, "raw-passthrough", &g_config.raw_passthrough, "Bypass all processing. Copies raw input bytes directly to output.", NULL, 0, 0),
        OPT_BOOLEAN(0, "iq-correction", &g_config.iq_correction.enable, "(Optional) Enable automatic I/Q imbalance correction.", NULL, 0, 0),
        OPT_BOOLEAN(0, "dc-block", &g_config.dc_block.enable, "(Optional) Enable DC offset removal (high-pass filter).", NULL, 0, 0),
        OPT_STRING(0, "preset", &g_config.preset_name, "Use a preset for a common target.", NULL, 0, 0),
    };

    #define DEFINE_CHAINABLE_FLOAT_OPTION(name, var, help_text) \
        OPT_FLOAT( 0, name,        &g_config.var[0], help_text, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-2",     &g_config.var[1], NULL, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-3",     &g_config.var[2], NULL, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-4",     &g_config.var[3], NULL, NULL, 0, 0), \
        OPT_FLOAT( 0, name "-5",     &g_config.var[4], NULL, NULL, 0, 0)
    #define DEFINE_CHAINABLE_STRING_OPTION(name, var, help_text) \
        OPT_STRING(0, name,        &g_config.var[0], help_text, NULL, 0, 0), \
        OPT_STRING(0, name "-2",     &g_config.var[1], NULL, NULL, 0, 0), \
        OPT_STRING(0, name "-3",     &g_config.var[2], NULL, NULL, 0, 0), \
        OPT_STRING(0, name "-4",     &g_config.var[3], NULL, NULL, 0, 0), \
        OPT_STRING(0, name "-5",     &g_config.var[4], NULL, NULL, 0, 0)
    static const struct argparse_option filter_options[] = {
        OPT_GROUP("Filtering Options (Chain up to 5 by combining options or adding suffixes -2, -3, etc. e.g., --lowpass --stopband --lowpass-2 --pass-range --pass-range-2)"),
        DEFINE_CHAINABLE_FLOAT_OPTION("lowpass", lowpass_cutoff_hz_arg, "Isolate signal at DC. Keeps freqs from -<hz> to +<hz>."),
        DEFINE_CHAINABLE_FLOAT_OPTION("highpass", highpass_cutoff_hz_arg, "Remove signal at DC. Rejects freqs from -<hz> to +<hz>."),
        DEFINE_CHAINABLE_STRING_OPTION("pass-range", pass_range_str_arg, "Isolate a specific band. Format: 'start_freq:end_freq'."),
        DEFINE_CHAINABLE_STRING_OPTION("stopband", stopband_str_arg, "Remove a specific band (notch). Format: 'start_freq:end_freq'."),
        OPT_GROUP("Filter Quality Options"),
        OPT_FLOAT(0, "transition-width", &g_config.transition_width_hz_arg, "Set filter sharpness by transition width in Hz. (Default: Auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-taps", &g_config.filter_taps_arg, "Set exact filter length. Overrides --transition-width.", NULL, 0, 0),
        OPT_FLOAT(0, "attenuation", &g_config.attenuation_db_arg, "Set filter stop-band attenuation in dB. (Default: 60).", NULL, 0, 0),
        OPT_GROUP("Filter Implementation Options (Advanced)"),
        OPT_STRING(0, "filter-type", &g_config.filter_type_str_arg, "Set filter implementation {fir|fft}. (Default: auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-fft-size", &g_config.filter_fft_size_arg, "Set FFT size for 'fft' filter type. Must be a power of 2.", NULL, 0, 0),
    };

    static const struct argparse_option sdr_general_options[] = {
        OPT_GROUP("SDR General Options"),
        OPT_FLOAT(0, "sdr-rf-freq", &g_config.sdr.rf_freq_hz_arg, "(Required for SDR) Tuner center frequency in Hz", NULL, 0, 0),
        OPT_FLOAT(0, "sdr-sample-rate", &g_config.sdr.sample_rate_hz_arg, "Set sample rate in Hz. (Device-specific default)", NULL, 0, 0),
        OPT_BOOLEAN(0, "sdr-bias-t", &g_config.sdr.bias_t_enable, "(Optional) Enable Bias-T power.", NULL, 0, 0),
    };

    static struct argparse_option final_options[] = {
        OPT_GROUP("Help & Version"),
        OPT_BOOLEAN('v', "version", NULL, "show program's version number and exit", version_cb, 0, OPT_NONEG),
        OPT_BOOLEAN('h', "help", NULL, "show this help message and exit", argparse_help_cb, 0, OPT_NONEG),
        OPT_END(),
    };

    #define APPEND_OPTIONS_MEMCPY(dest, src, n) \
        do { \
            if ((size_t)(total_opts + (n)) > (size_t)max_options) { \
                log_fatal("Internal error: Exceeded maximum number of CLI options."); \
                return -1; \
            } \
            memcpy(dest, src, (n) * sizeof(struct argparse_option)); \
            total_opts += (n); \
        } while (0)

    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], generic_options, sizeof(generic_options) / sizeof(generic_options[0]));
    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], filter_options, sizeof(filter_options) / sizeof(filter_options[0]));
    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], sdr_general_options, sizeof(sdr_general_options) / sizeof(sdr_general_options[0]));

    // Call the Module Manager service to add all module-specific options.
    // The complex logic of discovering and filtering options is now encapsulated there.
    // --- BUGFIX: Pass the base pointer 'options_buffer', not the offset pointer. ---
    module_manager_populate_cli_options(
        options_buffer,
        &total_opts,
        max_options,
        active_input_type,
        arena
    );

    if (config->num_presets > 0) {
        struct argparse_option preset_header[] = { OPT_GROUP("Available Presets") };
        APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], preset_header, 1);
        
        struct argparse_option preset_opts[MAX_PRESETS];
        int presets_to_add = (config->num_presets > MAX_PRESETS) ? MAX_PRESETS : config->num_presets;
        for (int i = 0; i < presets_to_add; i++) {
            preset_opts[i] = (struct argparse_option){
                .type = ARGPARSE_OPT_BOOLEAN,
                .long_name = config->presets[i].name,
                .help = config->presets[i].description,
                .flags = OPT_LONG_NOPREFIX,
            };
        }
        APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], preset_opts, presets_to_add);
    }

    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], final_options, sizeof(final_options) / sizeof(final_options[0]));

    return total_opts;
}


bool parse_arguments(int argc, char *argv[], AppConfig *config, MemoryArena* arena) {
    // MODIFIED: Save original arguments for better error messages in validation.
    g_original_argc = argc;
    g_original_argv = (const char**)argv;

    struct argparse_option all_options[MAX_TOTAL_OPTIONS];
    
    // The active input type is now passed down from main() to here.
    // We need it to build the context-sensitive list of options.
    const char* active_input_type = config->input_type_str;

    if (build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena, active_input_type) < 0) {
        return false;
    }

    struct argparse argparse;
    const char *const usages[] = { "iq_tool -i <type> [input_file] [options]", NULL, };
    argparse_init(&argparse, all_options, usages, 0);
    argparse_describe(&argparse, "\nResamples an I/Q file or a stream from an SDR device to a specified format and sample rate.", NULL);
    int non_opt_argc = argparse_parse(&argparse, argc, (const char **)argv);

    // After a successful parse, the input type string in g_config is now definitive.
    // We must re-check it in case the user provided a different one than our pre-scan found
    // (e.g. `-i wav -i rtlsdr`, argparse will take the last one).
    if (g_config.input_type_str && active_input_type && strcasecmp(g_config.input_type_str, active_input_type) != 0) {
        log_error("Multiple active modules provided.");
	return false;
    }

    if (!validate_and_process_args(config, non_opt_argc, argparse.out, arena)) {
        return false;
    }

    return true;
}

static bool validate_and_process_args(AppConfig *config, int non_opt_argc, const char** non_opt_argv, MemoryArena* arena) {
    // 1. Basic parsing result checks
    if (!config->input_type_str) {
        log_error("Missing required argument: --input <type>");
        return false;
    }

    ModuleInterface* selected_module_api = module_manager_get_input_interface_by_name(config->input_type_str, arena);
    if (!selected_module_api) {
        log_error("Invalid input type '%s'.", config->input_type_str);
        return false;
    }

    // 2. Handle non-option arguments (the input file path)
    bool is_file_input = (strcasecmp(config->input_type_str, "wav") == 0 ||
                          strcasecmp(config->input_type_str, "raw-file") == 0);

    if (is_file_input) {
        if (non_opt_argc == 0) {
            log_error("Missing <file_path> argument for '%s'.", config->input_type_str);
            return false;
        }
        if (non_opt_argc > 1) {
            log_error("Only one input file path is allowed.");
            return false;
        }
        config->input_filename_arg = (char*)non_opt_argv[0];
    } else {
        // MODIFIED: This entire block is replaced with the new, more intelligent error handling.
        if (non_opt_argc > 0) {
            const char* unexpected_arg = non_opt_argv[0];
            const char* preceding_arg = NULL;

            // Search for the unexpected argument in the original command line to find what came before it.
            for (int i = 1; i < g_original_argc; i++) {
                // Use pointer comparison for efficiency, as argparse gives us the original pointers.
                if (g_original_argv[i] == unexpected_arg) {
                    preceding_arg = g_original_argv[i - 1];
                    break;
                }
            }

            // If the preceding argument looks like an option, we have found the likely culprit.
            if (preceding_arg && preceding_arg[0] == '-') {
                log_error("Argument '%s' provided is not valid for the active module '%s'.",
                          preceding_arg, config->input_type_str);
            } else {
                // Fallback for a simple typo or a value without a preceding option.
                log_error("Unexpected argument '%s' provided for active module '%s'.",
                          unexpected_arg, config->input_type_str);
            }
            return false;
        }
    }

    // 3. Post-process SDR arguments
    if (config->sdr.rf_freq_hz_arg > 0.0f) {
        config->sdr.rf_freq_hz = (double)config->sdr.rf_freq_hz_arg;
        config->sdr.rf_freq_provided = true;
    }
    if (config->sdr.sample_rate_hz_arg > 0.0f) {
        config->sdr.sample_rate_hz = (double)config->sdr.sample_rate_hz_arg;
        config->sdr.sample_rate_provided = true;
    }

    // 4. Call all validation functions from the config module in the correct order
    if (selected_module_api->validate_options && !selected_module_api->validate_options(config)) return false;
    if (!validate_output_destination(config)) return false;
    if (!validate_output_type_and_sample_format(config)) return false;
    if (selected_module_api->validate_generic_options && !selected_module_api->validate_generic_options(config)) return false;
    if (!validate_filter_options(config)) return false;
    if (!validate_iq_correction_options(config)) return false;
    if (!validate_option_combinations(config)) return false;

    return true;
}
