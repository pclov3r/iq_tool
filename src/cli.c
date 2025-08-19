// src/cli.c

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
static bool validate_and_process_args(AppConfig *config, int non_opt_argc, const char** non_opt_argv);
static bool resolve_frequency_shift_options(AppConfig *config);
static bool validate_filter_options(AppConfig *config);
static bool validate_iq_correction_options(AppConfig *config);


void print_usage(const char *prog_name) {
    const char* help_argv[] = { prog_name, "--help" };
    parse_arguments(2, (char**)help_argv, &g_config);
}


bool parse_arguments(int argc, char *argv[], AppConfig *config) {
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

    static const struct argparse_option filter_options[] = {
        OPT_GROUP("Filtering Options"),
        OPT_FLOAT(0, "lowpass", &g_config.lowpass_cutoff_hz_arg, "Apply a low-pass filter, keeping frequencies from -<freq> to +<freq>.", NULL, 0, 0),
        OPT_FLOAT(0, "highpass", &g_config.highpass_cutoff_hz_arg, "Apply a high-pass filter, keeping frequencies above +<freq> and below -<freq>.", NULL, 0, 0),
        // CHANGE: Removed the implementation detail "with a FIR filter".
        OPT_STRING(0, "pass-range", &g_config.pass_range_str_arg, "Isolate a frequency range. Format: 'start_freq:end_freq' (e.g., '100e3:200e3').", NULL, 0, 0),
        OPT_STRING(0, "stopband", &g_config.stopband_str_arg, "Apply a stop-band (notch) filter. Format: 'start_freq:end_freq' (e.g., '-65:65').", NULL, 0, 0),
        OPT_GROUP("Filter Quality Options"),
        OPT_FLOAT(0, "transition-width", &g_config.transition_width_hz_arg, "Set filter sharpness by transition width in Hz. (Default: Auto).", NULL, 0, 0),
        OPT_INTEGER(0, "filter-taps", &g_config.filter_taps_arg, "Set exact filter length. Overrides --transition-width and auto mode.", NULL, 0, 0),
        OPT_FLOAT(0, "attenuation", &g_config.attenuation_db_arg, "Set filter stop-band attenuation in dB. (Default: 60).", NULL, 0, 0),
        OPT_GROUP("Filter Implementation Options"),
        OPT_STRING(0, "filter-type", &g_config.filter_type_str_arg, "Set filter implementation {fir|fft}. (Default: auto - fir for symmetric filters, fft for asymmetric)", NULL, 0, 0),
        OPT_INTEGER(0, "filter-fft-size", &g_config.filter_fft_size_arg, "Set FFT size for 'fft' filter type. Must be a power of 2. (Default: Auto)", NULL, 0, 0),
    };

    #if defined(ANY_SDR_SUPPORT_ENABLED)
    static const struct argparse_option sdr_general_options[] = {
        OPT_GROUP("SDR General Options"),
        OPT_FLOAT(0, "sdr-rf-freq", &g_config.sdr.rf_freq_hz_arg, "(Required for SDR) Tuner center frequency in Hz", NULL, 0, 0),
        OPT_FLOAT(0, "sdr-sample-rate", &g_config.sdr.sample_rate_hz_arg, "Set sample rate in Hz. (Device-specific default)", NULL, 0, 0),
        OPT_BOOLEAN(0, "sdr-bias-t", &g_config.sdr.bias_t_enable, "(Optional) Enable Bias-T power.", NULL, 0, 0),
    };
    #endif

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

    SAFE_MEMCPY(&all_options[total_opts], generic_options, sizeof(generic_options) / sizeof(generic_options[0]));
    SAFE_MEMCPY(&all_options[total_opts], filter_options, sizeof(filter_options) / sizeof(filter_options[0]));
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

    if (config->num_presets > 0) {
        struct argparse_option preset_header[] = { OPT_GROUP("Available Presets:") };
        SAFE_MEMCPY(&all_options[total_opts], preset_header, 1);
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
        SAFE_MEMCPY(&all_options[total_opts], preset_opts, presets_to_add);
    }

    SAFE_MEMCPY(&all_options[total_opts], final_options, sizeof(final_options) / sizeof(final_options[0]));

    struct argparse argparse;
    const char *const usages[] = { "iq_resample_tool -i <type> [input_file] [options]", NULL, };
    argparse_init(&argparse, all_options, usages, 0);
    argparse_describe(&argparse, "\nResamples an I/Q file or a stream from an SDR device to a specified format and sample rate.", NULL);
    int non_opt_argc = argparse_parse(&argparse, argc, (const char **)argv);

    if (!validate_and_process_args(config, non_opt_argc, argparse.out)) {
        return false;
    }

    return true;
}

static bool parse_start_end_string(const char* input_str, const char* arg_name, float* out_start, float* out_end) {
    char start_buf[128];
    char end_buf[128];

    if (sscanf(input_str, "%127[^:]:%127s", start_buf, end_buf) != 2) {
        log_fatal("Invalid format for %s. Expected 'start_freq:end_freq'. Found '%s'.", arg_name, input_str);
        return false;
    }

    char* endptr1;
    char* endptr2;
    *out_start = strtof(start_buf, &endptr1);
    *out_end = strtof(end_buf, &endptr2);

    if (*endptr1 != '\0' || *endptr2 != '\0') {
        log_fatal("Invalid numerical value in %s argument. Could not parse '%s'.", arg_name, input_str);
        return false;
    }

    if (*out_end <= *out_start) {
        log_fatal("In %s argument, end frequency must be greater than start frequency.", arg_name);
        return false;
    }

    return true;
}

static void add_filter_request(AppConfig *config, FilterType type, float f1, float f2) {
    if (config->num_filter_requests < MAX_FILTER_CHAIN) {
        config->filter_requests[config->num_filter_requests].type = type;
        config->filter_requests[config->num_filter_requests].freq1_hz = f1;
        config->filter_requests[config->num_filter_requests].freq2_hz = f2;
        config->num_filter_requests++;
    } else {
        log_warn("Maximum number of chained filters (%d) reached. Ignoring further filter options.", MAX_FILTER_CHAIN);
    }
}

static bool validate_filter_options(AppConfig *config) {
    config->num_filter_requests = 0;

    if (config->lowpass_cutoff_hz_arg > 0.0f) {
        add_filter_request(config, FILTER_TYPE_LOWPASS, config->lowpass_cutoff_hz_arg, 0.0f);
    }
    if (config->highpass_cutoff_hz_arg > 0.0f) {
        add_filter_request(config, FILTER_TYPE_HIGHPASS, config->highpass_cutoff_hz_arg, 0.0f);
    }
    if (config->pass_range_str_arg) {
        float start_f, end_f;
        if (!parse_start_end_string(config->pass_range_str_arg, "--pass-range", &start_f, &end_f)) return false;
        float bandwidth = end_f - start_f;
        float center_freq = start_f + (bandwidth / 2.0f);
        add_filter_request(config, FILTER_TYPE_PASSBAND, center_freq, bandwidth);
    }
    if (config->stopband_str_arg) {
        float start_f, end_f;
        if (!parse_start_end_string(config->stopband_str_arg, "--stopband", &start_f, &end_f)) return false;
        float bandwidth = end_f - start_f;
        float center_freq = start_f + (bandwidth / 2.0f);
        add_filter_request(config, FILTER_TYPE_STOPBAND, center_freq, bandwidth);
    }

    if (config->transition_width_hz_arg > 0.0f && config->filter_taps_arg > 0) {
        log_fatal("Error: Cannot specify both --transition-width and --filter-taps at the same time.");
        log_error("Please choose only one method to define the filter's quality.");
        return false;
    }

    if (config->transition_width_hz_arg < 0.0f) {
        log_fatal("--transition-width must be a positive value.");
        return false;
    }

    if (config->filter_taps_arg != 0 && config->filter_taps_arg < 3) {
        log_fatal("--filter-taps must be 3 or greater.");
        return false;
    }
    if (config->filter_taps_arg != 0 && config->filter_taps_arg % 2 == 0) {
        log_warn("--filter-taps must be an odd number. Adjusting from %d to %d.", config->filter_taps_arg, config->filter_taps_arg + 1);
        config->filter_taps_arg++;
    }

    if (config->attenuation_db_arg <= 0.0f && config->attenuation_db_arg != 0.0f) {
        log_fatal("--attenuation must be a positive value.");
        return false;
    }

    return true;
}

static bool resolve_frequency_shift_options(AppConfig *config) {
    if (config->freq_shift_hz_arg != 0.0f) {
        if (config->frequency_shift_request.type != FREQUENCY_SHIFT_REQUEST_NONE) {
            log_fatal("Conflicting frequency shift options provided. Cannot use --freq-shift and --wav-center-target-freq at the same time.");
            return false;
        }
        config->frequency_shift_request.type = FREQUENCY_SHIFT_REQUEST_MANUAL;
        config->frequency_shift_request.value = (double)config->freq_shift_hz_arg;
    }

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

    if (config->shift_after_resample && !config->freq_shift_requested) {
        log_fatal("Option --shift-after-resample was used, but no frequency shift was requested.");
        return false;
    }

    return true;
}

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

    config->frequency_shift_request.type = FREQUENCY_SHIFT_REQUEST_NONE;

    #if defined(ANY_SDR_SUPPORT_ENABLED)
    if (config->sdr.rf_freq_hz_arg > 0.0f) {
        config->sdr.rf_freq_hz = (double)config->sdr.rf_freq_hz_arg;
        config->sdr.rf_freq_provided = true;
    }
    if (config->sdr.sample_rate_hz_arg > 0.0f) {
        config->sdr.sample_rate_hz = (double)config->sdr.sample_rate_hz_arg;
        config->sdr.sample_rate_provided = true;
    }
    #endif

    if (selected_ops->validate_options) {
        if (!selected_ops->validate_options(config)) {
            return false;
        }
    }

    if (!validate_output_destination(config)) return false;
    if (!validate_output_type_and_sample_format(config)) return false;
    
    if (selected_ops->validate_generic_options) {
        if (!selected_ops->validate_generic_options(config)) {
            return false;
        }
    }
    
    if (!validate_filter_options(config)) return false;
    if (!resolve_frequency_shift_options(config)) return false;

    config->filter_type_request = FILTER_TYPE_FIR;
    if (config->filter_type_str_arg) {
        if (strcasecmp(config->filter_type_str_arg, "fir") == 0) {
            config->filter_type_request = FILTER_TYPE_FIR;
        } else if (strcasecmp(config->filter_type_str_arg, "fft") == 0) {
            config->filter_type_request = FILTER_TYPE_FFT;
        } else {
            log_fatal("Invalid value for --filter-type: '%s'. Must be 'fir' or 'fft'.", config->filter_type_str_arg);
            return false;
        }
    }

    if (config->filter_fft_size_arg != 0) {
        if (config->filter_type_request != FILTER_TYPE_FFT) {
            log_fatal("Option --filter-fft-size can only be used with --filter-type fft.");
            return false;
        }
        if (config->filter_fft_size_arg <= 0) {
            log_fatal("--filter-fft-size must be a positive integer.");
            return false;
        }
        int n = config->filter_fft_size_arg;
        if ((n > 0) && ((n & (n - 1)) != 0)) {
            log_fatal("--filter-fft-size must be a power of two (e.g., 1024, 2048, 4096).");
            return false;
        }
    }

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
        if (config->num_filter_requests > 0) {
            log_fatal("Option --raw-passthrough cannot be used with any filtering options.");
            return false;
        }
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

static bool validate_iq_correction_options(AppConfig *config) {
    if (config->iq_correction.enable) {
        if (!config->dc_block.enable) {
            log_fatal("Option --iq-correction requires --dc-block to be enabled for optimal performance and stability.");
            return false;
        }
    }
    return true;
}
