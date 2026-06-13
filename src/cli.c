/**
 * @file cli.c
 */

#include "cli.h"
#include "constants.h"
#include "app_context.h"
#include "config.h"
#include "log.h"
#include "utilities.h"
#include "platform.h"
#ifndef _WIN32
#include <libgen.h>
#include <limits.h>
#endif
#include "argparse.h"
#include "module_registry.h"
#include "agc.h"
#include "filter.h"
#include "sample_format_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

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
static bool validate_and_process_args(AppContext* app, int non_opt_argc, const char** non_opt_argv);
static int version_cb(struct argparse *self, const struct argparse_option *option);
static int build_cli_options(struct argparse_option* options_buffer, int max_options, AppConfig* config, MemoryArena* arena, const char* active_input_type, const char* active_output_type);
static void apply_preset_if_requested(AppConfig* config, MemoryArena* arena);

// --- Callback to catch users trying to use presets as flags ---
static int preset_flag_warning_cb(struct argparse *self, const struct argparse_option *option) {
    (void)self;
    log_error("'--%s' is not a valid flag. To load the '%s' preset, use '--preset %s'.",
              option->long_name, option->long_name, option->long_name);
    exit(EXIT_FAILURE);
    return 0;
}

void cli_print_usage(const char *prog_name, AppConfig *config, MemoryArena* arena) {
    (void)prog_name;
    struct argparse argparse;
    struct argparse_option all_options[MAX_TOTAL_OPTIONS];
    const char *const usages[] = {
        "iq_tool -i <in_type> [in_file] -o <out_type> [out_file] [options]",
        NULL,
    };

    build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena, NULL, NULL);
    argparse_init(&argparse, all_options, usages, 0);
    argparse_describe(&argparse, NULL, NULL);
    argparse_usage(&argparse);
}

static int version_cb(struct argparse *self, const struct argparse_option *option) {
    (void)self; (void)option;
#ifdef GIT_HASH
    fprintf(stdout, "%s version %s\n", APP_NAME, GIT_HASH);
#else
    fprintf(stdout, "%s version unknown\n", APP_NAME);
#endif
    exit(EXIT_SUCCESS);
}

static int build_cli_options(struct argparse_option* options_buffer, int max_options, AppConfig* config, MemoryArena* arena, const char* active_input_type, const char* active_output_type) {
    int total_opts = 0;
    struct argparse_option generic_options[] = {
        OPT_GROUP("Required Input & Output"),
        OPT_STRING('i', "input", &config->input.type_name, "Specifies the input module.", NULL, 0, 0),
        OPT_STRING('o', "output", &config->output.module_name, "Specifies the output module and optional file path", NULL, 0, 0),

        OPT_GROUP("Audio Output Options"),
        OPT_STRING(0, "audio-writer", &config->audio.path_arg, "Save demodulated audio to a WAV file.", NULL, 0, 0),
        OPT_BOOLEAN(0, "audio-writer-rf64", &config->audio.writer_rf64, "Use RF64 format for the audio writer (supports >4GB files).", NULL, 0, 0),
        OPT_BOOLEAN(0, "mute-audio", &config->audio.mute, "Disable speaker playback", NULL, 0, 0),

        OPT_GROUP("Input Processing Options"),
        OPT_FLOAT(0, "input-gain-multiplier", &config->dsp.input_gain, "Apply a linear gain multiplier to INPUT samples (before processing).", NULL, 0, 0),
        OPT_BOOLEAN(0, "iq-correction", &config->dsp.iq_correction.enable, "(Optional) Enable automatic I/Q imbalance correction.", NULL, 0, 0),
        OPT_BOOLEAN(0, "dc-block", &config->dsp.dc_block.enable, "(Optional) Enable DC offset removal (high-pass filter).", NULL, 0, 0),

        OPT_GROUP("Baseband Processing Options"),
        OPT_DOUBLE(0, "baseband-sample-rate", &config->baseband_sample_rate.user_arg, "Baseband sample rate feeding into a demodulator in Hz.", NULL, 0, 0),
        OPT_STRING(0, "baseband-sample-format", &config->baseband_sample_format.format_str, "Baseband sample format feeding into a demodulator {cf32|cs16|...}.", NULL, 0, 0),
        OPT_FLOAT(0, "baseband-gain-multiplier", &config->dsp.baseband_gain, "Apply a linear gain multiplier to baseband samples before demodulation.", NULL, 0, 0),
        OPT_BOOLEAN(0, "baseband-agc", &config->dsp.baseband_agc.enable, "Enable automatic gain control on the baseband signal before demodulation.", NULL, 0, 0),
        OPT_FLOAT(0, "baseband-agc-target", &config->dsp.baseband_agc.target_level_arg, "AGC target magnitude (0.0 - 1.0). (Default: 0.12)", NULL, 0, 0),


        OPT_GROUP("I/Q Output Options"),
        OPT_DOUBLE(0, "output-sample-rate", &config->output_sample_rate.user_arg, "Output sample rate in Hz.", NULL, 0, 0),
        OPT_STRING(0, "output-sample-format", &config->output.sample_format_str, "Sample format for output data {cs8|cu8|cs16|...}.", NULL, 0, 0),
        OPT_FLOAT(0, "output-gain-multiplier", &config->dsp.output_gain, "Apply a linear gain multiplier to OUTPUT samples before saving.", NULL, 0, 0),
        OPT_BOOLEAN(0, "output-agc", &config->dsp.output_agc.enable, "Enable automatic gain control on the output signal before saving.", NULL, 0, 0),
        OPT_FLOAT(0, "output-agc-target", &config->dsp.output_agc.target_level_arg, "AGC target magnitude (0.0 - 1.0). (Default: 0.12)", NULL, 0, 0),

        OPT_GROUP("General Pipeline Options"),
        OPT_DOUBLE(0, "freq-shift", &config->dsp.frequency_shift_hz, "Apply a direct frequency shift in Hz (e.g., -100e3)", NULL, 0, 0),
        OPT_BOOLEAN(0, "shift-after-resample", &config->dsp.shift_after_resample, "Apply frequency shift AFTER resampling (default is before)", NULL, 0, 0),
        OPT_BOOLEAN(0, "raw-passthrough", &config->dsp.raw_passthrough, "Bypass all processing. Copies raw input bytes directly to output.", NULL, 0, 0),
        OPT_STRING(0, "preset", &config->preset_name, "Use a preset for a common target.", NULL, 0, 0),
    };

    struct argparse_option sdr_general_options[] = {
        OPT_GROUP("SDR General Options"),
        OPT_DOUBLE(0, "sdr-rf-freq", &config->sdr_general.rf_freq_hz_arg, "(Required for SDR) Tuner center frequency in Hz", NULL, 0, 0),
        OPT_DOUBLE(0, "sdr-freq-offset", &config->sdr_general.frequency_offset_arg, "Frequency offset in Hz (e.g. 125e6 for HamItUp).", NULL, 0, 0),
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
                log_fatal("Internal error: Exceeded maximum number of CLI options."); return -1; \
            } \
            memcpy(dest, src, (n) * sizeof(struct argparse_option)); \
            total_opts += (n); \
        } while (0)

    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], generic_options, sizeof(generic_options) / sizeof(generic_options[0]));
    total_opts += filter_populate_cli_options(&options_buffer[total_opts], config);
    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], sdr_general_options, sizeof(sdr_general_options) / sizeof(sdr_general_options[0]));
    module_populate_cli_options(options_buffer, &total_opts, max_options, active_input_type, active_output_type, arena);
    if (config->num_presets > 0) {
        struct argparse_option preset_header[] = { OPT_GROUP("Available Presets") };
        APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], preset_header, 1);
        struct argparse_option preset_opts[MAX_PRESETS];
        int presets_to_add = (config->num_presets > MAX_PRESETS) ? MAX_PRESETS : config->num_presets;
        for (int i = 0; i < presets_to_add; i++) {
            preset_opts[i] = (struct argparse_option){ .type = ARGPARSE_OPT_BOOLEAN, .long_name = config->presets[i].name, .help = config->presets[i].description, .flags = OPT_LONG_NOPREFIX, .callback = preset_flag_warning_cb, .value = NULL };
        }
        APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], preset_opts, presets_to_add);
    }
    APPEND_OPTIONS_MEMCPY(&options_buffer[total_opts], final_options, sizeof(final_options) / sizeof(final_options[0]));
    return total_opts;
}

bool cli_parse(int argc, char *argv[], AppContext *app) {
    AppConfig *config = (AppConfig*)app->config;
    MemoryArena *arena = &app->pipeline.setup_arena;
    g_original_argc = argc;
    g_original_argv = (const char**)argv;

    // --- Targeted Pre-scan ---
    const char* pre_input = NULL;
    const char* pre_output = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) && i + 1 < argc) pre_input = argv[i+1];
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) pre_output = argv[i+1];
    }
    if (pre_input) {
        const Module* mod = module_get(pre_input, MODULE_TYPE_INPUT, arena);
        if (mod && mod->set_default_config) mod->set_default_config(config);
    }
    if (pre_output) {
        const Module* mod = module_get(pre_output, MODULE_TYPE_OUTPUT, arena);
        if (mod && mod->set_default_config) mod->set_default_config(config);
    }

    struct argparse_option all_options[MAX_TOTAL_OPTIONS];
    if (build_cli_options(all_options, MAX_TOTAL_OPTIONS, config, arena, pre_input, pre_output) < 0) {
        return false;
    }

    struct argparse argparse;
    const char *const usages[] = { "iq_tool -i <in_type> [in_file] -o <out_type> [out_file] [options]", NULL, };
    argparse_init(&argparse, all_options, usages, 0);
    argparse_describe(&argparse, NULL, NULL);
    int non_opt_argc = argparse_parse(&argparse, argc, (const char **)argv);

    // Apply presets first, so user flags can override them.
    apply_preset_if_requested(config, arena);

    if (!validate_and_process_args(app, non_opt_argc, argparse.out)) return false;

    return true;
}

static void apply_preset_if_requested(AppConfig* config, MemoryArena* arena) {
    (void)arena;
    if (!config->preset_name) return;

    for (int i = 0; i < config->num_presets; i++) {
        if (strcasecmp(config->preset_name, config->presets[i].name) == 0) {
            const PresetDefinition* p = &config->presets[i];
            if (p->rate_hz_provided) {
                config->output_sample_rate.rate_hz = p->rate_hz;
                config->output_sample_rate.provided = true;
            }
            if (p->baseband_sample_rate_provided) {
                config->baseband_sample_rate.rate_hz = p->baseband_sample_rate_hz;
                config->baseband_sample_rate.provided = true;
            }
            if (!config->output.sample_format_str) config->output.sample_format_str = p->output_sample_format;
            if (!config->baseband_sample_format.format_str) config->baseband_sample_format.format_str = p->baseband_sample_format;
            if (p->input_gain_provided && config->dsp.input_gain == 1.0f) config->dsp.input_gain = p->input_gain;
            if (p->output_gain_provided && config->dsp.output_gain == 1.0f) config->dsp.output_gain = p->output_gain;
            if (p->baseband_gain_provided && config->dsp.baseband_gain == 1.0f) config->dsp.baseband_gain = p->baseband_gain;
            if (p->dc_block_provided && !config->dsp.dc_block.enable) config->dsp.dc_block.enable = p->dc_block_enable;
            if (p->iq_correction_provided && !config->dsp.iq_correction.enable) config->dsp.iq_correction.enable = p->iq_correction_enable;
            if (p->output_agc_enable_provided && !config->dsp.output_agc.enable) config->dsp.output_agc.enable = p->output_agc_enable;
            if (p->output_agc_target_provided && config->dsp.output_agc.target_level_arg == 0.0f) config->dsp.output_agc.target_level_arg = p->output_agc_target;
            if (p->baseband_agc_enable_provided && !config->dsp.baseband_agc.enable) config->dsp.baseband_agc.enable = p->baseband_agc_enable;
            if (p->baseband_agc_target_provided && config->dsp.baseband_agc.target_level_arg == 0.0f) config->dsp.baseband_agc.target_level_arg = p->baseband_agc_target;
            if (p->lowpass_cutoff_hz_provided && config->dsp.filter.args.lowpass[0] == 0.0f) config->dsp.filter.args.lowpass[0] = p->lowpass_cutoff_hz;
            if (p->highpass_cutoff_hz_provided && config->dsp.filter.args.highpass[0] == 0.0f) config->dsp.filter.args.highpass[0] = p->highpass_cutoff_hz;
            if (p->pass_range_str_provided && !config->dsp.filter.args.pass_range[0]) config->dsp.filter.args.pass_range[0] = p->pass_range_str;
            if (p->stopband_str_provided && !config->dsp.filter.args.stopband[0]) config->dsp.filter.args.stopband[0] = p->stopband_str;
            if (p->transition_width_hz_provided && config->dsp.filter.args.transition_width == 0.0f) config->dsp.filter.args.transition_width = p->transition_width_hz;
            if (p->filter_taps_provided && config->dsp.filter.args.taps == 0) config->dsp.filter.args.taps = p->filter_taps;
            if (p->default_filter_attenuation_db_provided && config->dsp.filter.args.attenuation == 0.0f) config->dsp.filter.args.attenuation = p->default_filter_attenuation_db;
            if (p->filter_type_str_provided && !config->dsp.filter.args.type_str) config->dsp.filter.args.type_str = p->filter_type_str;
            return;
        }
    }
    log_error("Unknown preset '%s'. Check '%s' or --help for available presets.", config->preset_name, PRESETS_FILENAME);
    exit(EXIT_FAILURE);
}

static bool resolve_file_paths(AppConfig *config, MemoryArena* arena) {
    if (!config || !arena) return false;
#ifdef _WIN32
    if (config->input.path_arg) {
        if (!get_absolute_path_windows(config->input.path_arg, config->input.effective_path_w, MAX_PATH_BUFFER, config->input.effective_path_utf8, MAX_PATH_BUFFER)) return false;
    }
    if (config->output.path_arg) {
        if (!get_absolute_path_windows(config->output.path_arg, config->output.effective_path_w, MAX_PATH_BUFFER, config->output.effective_path_utf8, MAX_PATH_BUFFER)) return false;
    }
    if (config->audio.path_arg) {
        if (!get_absolute_path_windows(config->audio.path_arg, config->audio.effective_path_w, MAX_PATH_BUFFER, config->audio.effective_path_utf8, MAX_PATH_BUFFER)) return false;
    }
#else
    if (config->input.path_arg) {
        char resolved_input_path[PATH_MAX];
        if (realpath(config->input.path_arg, resolved_input_path) == NULL) {
            log_error("Input file not found or path is invalid: %s (%s)", config->input.path_arg, strerror(errno));
            return false;
        }
        config->input.effective_path = mem_arena_alloc(arena, strlen(resolved_input_path) + 1, false);
        if (!config->input.effective_path) return false;
        strcpy(config->input.effective_path, resolved_input_path);
    }
    if (config->output.path_arg) {
        char* path_copy_for_dirname = mem_arena_alloc(arena, strlen(config->output.path_arg) + 1, false);
        char* path_copy_for_basename = mem_arena_alloc(arena, strlen(config->output.path_arg) + 1, false);
        if (!path_copy_for_dirname || !path_copy_for_basename) return false;
        strcpy(path_copy_for_dirname, config->output.path_arg);
        strcpy(path_copy_for_basename, config->output.path_arg);
        char* dir = dirname(path_copy_for_dirname);
        char* base = basename(path_copy_for_basename);
        char resolved_dir_path[PATH_MAX];
        if (realpath(dir, resolved_dir_path) == NULL) {
            log_error("Output directory does not exist or path is invalid: %s (%s)", dir, strerror(errno));
            return false;
        }
        size_t final_length = strlen(resolved_dir_path) + 1 + strlen(base) + 1;
        config->output.effective_path = mem_arena_alloc(arena, final_length, false);
        if (!config->output.effective_path) return false;
        snprintf(config->output.effective_path, final_length, "%s/%s", resolved_dir_path, base);
    }
    if (config->audio.path_arg) {
        char* path_copy_for_dirname = mem_arena_alloc(arena, strlen(config->audio.path_arg) + 1, false);
        char* path_copy_for_basename = mem_arena_alloc(arena, strlen(config->audio.path_arg) + 1, false);
        if (!path_copy_for_dirname || !path_copy_for_basename) return false;
        strcpy(path_copy_for_dirname, config->audio.path_arg);
        strcpy(path_copy_for_basename, config->audio.path_arg);
        char* dir = dirname(path_copy_for_dirname);
        char* base = basename(path_copy_for_basename);
        char resolved_dir_path[PATH_MAX];
        if (realpath(dir, resolved_dir_path) == NULL) {
            log_error("Audio writer directory does not exist or path is invalid: %s (%s)", dir, strerror(errno));
            return false;
        }
        size_t final_length = strlen(resolved_dir_path) + 1 + strlen(base) + 1;
        config->audio.effective_path = mem_arena_alloc(arena, final_length, false);
        if (!config->audio.effective_path) return false;
        snprintf(config->audio.effective_path, final_length, "%s/%s", resolved_dir_path, base);
    }
#endif
    return true;
}

static bool validate_and_process_args(AppContext *app, int non_opt_argc, const char** non_opt_argv) {
    AppConfig* config = (AppConfig*)app->config;
    MemoryArena* arena = &app->pipeline.setup_arena;
    if (!config->input.type_name) {
        log_error("Missing required argument: --input <type>");
        return false;
    }
    const Module* selected_input_module = module_get(config->input.type_name, MODULE_TYPE_INPUT, arena);
    if (!selected_input_module) {
        log_error("Invalid input type '%s'.", config->input.type_name);
        return false;
    }
    InputModuleInterface* selected_module_api = (InputModuleInterface*)selected_input_module->api;
    if (!selected_module_api) return false;

    if (selected_input_module->requires_input_path) {
        if (non_opt_argc < 1) {
            log_error("Missing <in_file> argument for input type '%s'.", config->input.type_name);
            return false;
        }
        config->input.path_arg = (char*)non_opt_argv[0];
        non_opt_argv++; non_opt_argc--;
    }

    if (!config->output.module_name) {
        log_error("Missing required argument: --output <type> [path]");
        return false;
    }
    const Module* selected_output_module = module_get(config->output.module_name, MODULE_TYPE_OUTPUT, arena);
    if (!selected_output_module) {
        log_error("Invalid value for --output: '%s'.", config->output.module_name);
        return false;
    }
    if (selected_output_module->requires_output_path) {
        if (non_opt_argc < 1) {
            log_error("Missing <out_file> argument for '--output %s'.", config->output.module_name);
            return false;
        }
        config->output.path_arg = (char*)non_opt_argv[0];
        non_opt_argv++; non_opt_argc--;
    } else {
        config->output.path_arg = NULL;
    }

    if (non_opt_argc > 0) {
        log_error("Unexpected argument: '%s'", non_opt_argv[0]);
        return false;
    }

    if (config->output_sample_rate.user_arg > 0.0f) {
        config->output_sample_rate.rate_hz = (double)config->output_sample_rate.user_arg;
        config->output_sample_rate.provided = true;
    }
    if (config->baseband_sample_rate.user_arg > 0.0f) {
        config->baseband_sample_rate.rate_hz = (double)config->baseband_sample_rate.user_arg;
        config->baseband_sample_rate.provided = true;
    }
    if (config->sdr_general.rf_freq_hz_arg > 0.0f) {
        config->sdr_general.rf_freq_hz = config->sdr_general.rf_freq_hz_arg;
        config->sdr_general.rf_freq_provided = true;
    }
    if (config->sdr_general.frequency_offset_arg != 0.0f) {
        config->sdr_general.frequency_offset_hz = config->sdr_general.frequency_offset_arg;
    }
    if (config->sdr_general.sample_rate_hz_arg > 0.0f) {
        config->sdr_general.sample_rate_hz = config->sdr_general.sample_rate_hz_arg;
        config->sdr_general.sample_rate_provided = true;
    }

    if (!resolve_file_paths(config, arena)) return false;

    // --- Validate Audio Writer Early ---
#ifdef _WIN32
    if (config->audio.effective_path_utf8[0] != '\0') {
        if (!utility_verify_output_path(config, config->audio.effective_path_utf8)) return false;
    }
#else
    if (config->audio.effective_path) {
        if (!utility_verify_output_path(config, config->audio.effective_path)) return false;
    }
#endif

    // --- Final Validation Cascade ---
    if (!validate_output_type_and_sample_format(config)) return false;
    if (selected_module_api->validate_options && !selected_module_api->validate_options(app)) return false;
    if (selected_module_api->validate_generic_options && !selected_module_api->validate_generic_options(config)) return false;

    const Module* out_mod_val = module_get(config->output.module_name, MODULE_TYPE_OUTPUT, arena);
    if (out_mod_val) {
        OutputModuleInterface* out_api = (OutputModuleInterface*)out_mod_val->api;
        if (out_api && out_api->validate_options) {
            if (!out_api->validate_options(app)) return false;
        }
    }

    if (!validate_filter_options(config)) return false;
    if (!validate_iq_correction_options(config)) return false;
    if (!validate_option_combinations(config)) return false;

    return true;
}
