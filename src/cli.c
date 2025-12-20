#include "cli.h"
#include "constants.h"
#include "app_context.h"      // Provides AppConfig, MemoryArena
#include "config.h"           // Provides validation function prototypes
#include "log.h"
#include "utils.h"
#include "argparse.h"
#include "module_manager.h"
#include "agc.h"              // NEW: For agc_populate_cli_options
#include "filter.h"           // NEW: For filter_populate_cli_options
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

// Store original command-line arguments for improved error reporting.
static int g_original_argc = 0;
static const char** g_original_argv = NULL;

#define MAX_STATIC_OPTIONS 128
#define MAX_TOTAL_OPTIONS (MAX_STATIC_OPTIONS + MAX_PRESETS)

// --- Forward Declarations ---
static bool validate_and_process_args(AppConfig *config, int non_opt_argc, const char** non_opt_argv, MemoryArena* arena);
static int version_cb(struct argparse *self, const struct argparse_option *option);
static int build_cli_options(struct argparse_option* options_buffer, int max_options, AppConfig* config, MemoryArena* arena, const char* active_input_type);

// --- Callback to catch users trying to use presets as flags ---
static int preset_flag_warning_cb(struct argparse *self, const struct argparse_option *option) {
    (void)self;
    log_error("'--%s' is not a valid flag. To load the '%s' preset, use '--preset %s'.",
              option->long_name, option->long_name, option->long_name);
    exit(EXIT_FAILURE);
    return 0;
}

void print_usage(const char *prog_name, AppConfig *config, MemoryArena* arena) {
    (void)prog_name;
    struct argparse argparse;
    struct argparse_option all_options[MAX_TOTAL_OPTIONS];
    const char *const usages[] = {
        "iq_tool -i <in_type> [in_file] -o <out_type> [out_file] [options]",
        NULL,
    };

    // Build the full options list to generate complete help text.
    build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena, NULL);

    argparse_init(&argparse, all_options, usages, 0);
    // CHANGE: Removed description string.
    argparse_describe(&argparse, NULL, NULL);
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

    struct argparse_option generic_options[] = {
        OPT_GROUP("Required Input & Output"),
        OPT_STRING('i', "input", &config->input.type_name, "Specifies the input module.", NULL, 0, 0),
        OPT_STRING('o', "output", &config->output.module_name, "Specifies the output module and optional file path", NULL, 0, 0),
        OPT_GROUP("Output Options"),
        OPT_STRING(0, "output-sample-format", &config->output.format_name, "Sample format for output data {cs8|cu8|cs16|...}", NULL, 0, 0),
        OPT_GROUP("Processing Options"),
        OPT_DOUBLE(0, "output-rate", &config->output_rate.user_arg, "Output sample rate in Hz.", NULL, 0, 0),
        OPT_FLOAT(0, "input-gain-multiplier", &config->dsp.input_gain, "Apply a linear gain multiplier to INPUT samples (before processing).", NULL, 0, 0),
        OPT_FLOAT(0, "output-gain-multiplier", &config->dsp.output_gain, "Apply a linear gain multiplier to OUTPUT samples (after processing).", NULL, 0, 0),
        OPT_DOUBLE(0, "freq-shift", &config->dsp.freq_shift_hz, "Apply a direct frequency shift in Hz (e.g., -100e3)", NULL, 0, 0),
        OPT_BOOLEAN(0, "shift-after-resample", &config->dsp.shift_after_resample, "Apply frequency shift AFTER resampling (default is before)", NULL, 0, 0),
        OPT_BOOLEAN(0, "raw-passthrough", &config->dsp.raw_passthrough, "Bypass all processing. Copies raw input bytes directly to output.", NULL, 0, 0),
        OPT_BOOLEAN(0, "iq-correction", &config->dsp.iq_correction.enable, "(Optional) Enable automatic I/Q imbalance correction.", NULL, 0, 0),
        OPT_BOOLEAN(0, "dc-block", &config->dsp.dc_block.enable, "(Optional) Enable DC offset removal (high-pass filter).", NULL, 0, 0),
        OPT_STRING(0, "preset", &config->preset_name, "Use a preset for a common target.", NULL, 0, 0),
    };

    struct argparse_option sdr_general_options[] = {
        OPT_GROUP("SDR General Options"),
        OPT_DOUBLE(0, "sdr-rf-freq", &config->sdr_general.rf_freq_hz_arg, "(Required for SDR) Tuner center frequency in Hz", NULL, 0, 0),
        OPT_DOUBLE(0, "sdr-frequency-offset", &config->sdr_general.frequency_offset_arg, "Frequency offset in Hz (e.g. 125e6 for HamItUp).", NULL, 0, 0),
        OPT_DOUBLE(0, "sdr-sample-rate", &config->sdr_general.sample_rate_hz_arg, "Set sample rate in Hz. (Device-specific default)", NULL, 0, 0),
        OPT_BOOLEAN(0, "sdr-bias-t", &config->sdr_general.bias_t_enable, "(Optional) Enable Bias-T power.", NULL, 0, 0),
    };

    struct argparse_option final_options[] = {
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

    // 1. Add Generic Options
    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], generic_options, sizeof(generic_options) / sizeof(generic_options[0]));

    // 2. Delegate to Subsystems
    total_opts += agc_populate_cli_options(&options_buffer[total_opts], config);
    total_opts += filter_populate_cli_options(&options_buffer[total_opts], config);

    // 3. Add SDR General Options
    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], sdr_general_options, sizeof(sdr_general_options) / sizeof(sdr_general_options[0]));

    // 4. Delegate to Modules (Inputs/Outputs)
    module_manager_populate_cli_options(
        options_buffer,
        &total_opts,
        max_options,
        active_input_type,
        arena
    );

    // 5. Add Presets
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
                .callback = preset_flag_warning_cb,
                .value = NULL
            };
        }
        APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], preset_opts, presets_to_add);
    }

    // 6. Add Final Options (Help/Version)
    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], final_options, sizeof(final_options) / sizeof(final_options[0]));

    return total_opts;
}


bool parse_arguments(int argc, char *argv[], AppConfig *config, MemoryArena* arena) {
    g_original_argc = argc;
    g_original_argv = (const char**)argv;

    struct argparse_option all_options[MAX_TOTAL_OPTIONS];
    
    const char* active_input_type = config->input.type_name;

    if (build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena, active_input_type) < 0) {
        return false;
    }

    struct argparse argparse;
    const char *const usages[] = { "iq_tool -i <in_type> [in_file] -o <out_type> [out_file] [options]", NULL, };
    argparse_init(&argparse, all_options, usages, 0);
    // CHANGE: Removed description string.
    argparse_describe(&argparse, NULL, NULL);
    int non_opt_argc = argparse_parse(&argparse, argc, (const char **)argv);

    if (config->input.type_name && active_input_type && strcasecmp(config->input.type_name, active_input_type) != 0) {
        log_error("Multiple active modules provided.");
        return false;
    }

    if (!validate_and_process_args(config, non_opt_argc, argparse.out, arena)) {
        return false;
    }

    return true;
}

static bool validate_and_process_args(AppConfig *config, int non_opt_argc, const char** non_opt_argv, MemoryArena* arena) {
    // --- Step 1: Validate Input Module and its Arguments ---
    if (!config->input.type_name) {
        log_error("Missing required argument: --input <type>");
        return false;
    }

    // Lookup the full module definition to check capabilities/requirements
    const Module* selected_input_module = module_manager_get_module_by_name(config->input.type_name, arena);

    if (!selected_input_module || selected_input_module->type != MODULE_TYPE_INPUT) {
        log_error("Invalid input type '%s'.", config->input.type_name);
        return false;
    }

    // We still need the API pointer for later validation steps in this function
    InputModuleInterface* selected_module_api = (InputModuleInterface*)selected_input_module->api;

    // Check if this specific input module requires a positional argument (file path)
    if (selected_input_module->requires_input_path) {
        if (non_opt_argc < 1) {
            log_error("Missing <in_file> argument for input type '%s'.", config->input.type_name);
            return false;
        }
        config->input.path_arg = (char*)non_opt_argv[0];
        // Consume the argument
        non_opt_argv++;
        non_opt_argc--;
    }

    // --- Step 2: Validate Output Module and its Arguments ---
    if (!config->output.module_name) {
        log_fatal("Missing required argument: --output <type> [path]");
        return false;
    }

    const Module* selected_output_module = module_manager_get_output_module_by_name(config->output.module_name, arena);
    if (!selected_output_module) {
        log_fatal("Invalid value for --output: '%s'.", config->output.module_name);
        return false;
    }

    if (selected_output_module->requires_output_path) {
        if (non_opt_argc < 1) {
            log_fatal("Missing <out_file> argument for '--output %s'.", config->output.module_name);
            return false;
        }
        config->output.path_arg = (char*)non_opt_argv[0];
        // Consume the argument
        non_opt_argv++;
        non_opt_argc--;
    } else { // Module does not require a path (e.g., stdout)
        config->output.path_arg = NULL;
    }

    // --- Step 3: Check for Unexpected Extra Arguments ---
    if (non_opt_argc > 0) {
        const char* unexpected_arg = non_opt_argv[0];
        const char* preceding_arg = NULL;

        for (int i = 1; i < g_original_argc; i++) {
            if (g_original_argv[i] == unexpected_arg) {
                preceding_arg = g_original_argv[i - 1];
                break;
            }
        }

        if (preceding_arg && preceding_arg[0] == '-') {
            log_error("Argument '%s' provided is not valid for the active module '%s'.", preceding_arg, config->input.type_name);
        } else {
            log_error("Unexpected argument: '%s'", unexpected_arg);
        }
        return false;
    }

    // --- Step 4: Post-process SDR arguments ---
    if (config->sdr_general.rf_freq_hz_arg > 0.0f) {
        config->sdr_general.rf_freq_hz = config->sdr_general.rf_freq_hz_arg;

        // Apply Frequency Offset Logic
        if (config->sdr_general.frequency_offset_arg != 0.0f) {
            config->sdr_general.frequency_offset_hz = config->sdr_general.frequency_offset_arg;

            // 1. PERFORM MATH FIRST: Target + Offset = Hardware
            config->sdr_general.rf_freq_hz += config->sdr_general.frequency_offset_hz;

            // 2. LOG RESULT
            log_info("Applying Frequency Offset: Target %.0f Hz + Offset %+.0f Hz = Tuning to %.0f Hz",
                     config->sdr_general.rf_freq_hz - config->sdr_general.frequency_offset_hz,
                     config->sdr_general.frequency_offset_hz,
                     config->sdr_general.rf_freq_hz);

            // Sanity check the resulting hardware frequency
            if (config->sdr_general.rf_freq_hz <= 0.0) {
                double user_target = config->sdr_general.rf_freq_hz - config->sdr_general.frequency_offset_hz;
                log_error("Calculated hardware frequency is %.0f Hz (must be positive).", config->sdr_general.rf_freq_hz);
                log_error("Target: %.0f Hz, Offset: %+.0f Hz", user_target, config->sdr_general.frequency_offset_hz);
                return false;
            }
        }

        config->sdr_general.rf_freq_provided = true;
    }
    if (config->sdr_general.sample_rate_hz_arg > 0.0f) {
        config->sdr_general.sample_rate_hz = config->sdr_general.sample_rate_hz_arg;
        config->sdr_general.sample_rate_provided = true;
    }

    // --- Step 5: Call all validation functions from the config module ---
    if (selected_module_api->validate_options && !selected_module_api->validate_options(config)) return false;

    if (!validate_output_type_and_sample_format(config)) return false;

    // --- Validate Output Module Options (Early Check) ---
    // We look up the output module again to get its API.
    // This allows modules like 'nrsc5' to check for missing flags (program_id)
    // AND override the sample rate/format set in Step 2 if necessary.
    const Module* out_mod_val = module_manager_get_output_module_by_name(config->output.module_name, arena);
    if (out_mod_val) {
        OutputModuleInterface* out_api = (OutputModuleInterface*)out_mod_val->api;
        if (out_api->validate_options) {
            if (!out_api->validate_options(config)) return false;
        }
    }

    if (selected_module_api->validate_generic_options && !selected_module_api->validate_generic_options(config)) return false;
    if (!validate_filter_options(config)) return false;
    if (!validate_iq_correction_options(config)) return false;
    if (!validate_option_combinations(config)) return false;

    return true;
}
