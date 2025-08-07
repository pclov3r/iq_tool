// cli.c

#include "cli.h"
#include "types.h"
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <limits.h> // For INT_MIN, INT_MAX

#ifdef _WIN32
#include <getopt.h>
#else
#include <unistd.h>
#include <getopt.h>
#include <strings.h> // For strcasecmp
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// --- Add an external declaration for the global config defined in main.c ---
extern AppConfig g_config;

// --- Enum for long options without a short equivalent ---
enum {
    OPT_START = 256,
    OPT_GAIN, OPT_OUTPUT_CONTAINER, OPT_OUTPUT_SAMPLE_FORMAT, OPT_OUTPUT_RATE,
    OPT_NO_RESAMPLE, OPT_WAV_CENTER_TARGET_FREQUENCY,
    OPT_WAV_SHIFT_FREQUENCY, OPT_WAV_SHIFT_AFTER_RESAMPLE, OPT_RAW_FILE_INPUT_RATE,
    OPT_RAW_FILE_INPUT_SAMPLE_FORMAT, OPT_SDR_RF_FREQ, OPT_SDR_BIAS_T,
    OPT_SDRPLAY_DEVICE_IDX, OPT_SDRPLAY_GAIN_LEVEL, OPT_SDRPLAY_ANTENNA,
    OPT_SDRPLAY_HDR_MODE, OPT_SDRPLAY_HDR_BW, OPT_SDRPLAY_SAMPLE_RATE,
    OPT_SDRPLAY_BANDWIDTH, OPT_HACKRF_LNA_GAIN, OPT_HACKRF_VGA_GAIN,
    OPT_HACKRF_AMP_ENABLE, OPT_HACKRF_SAMPLE_RATE,
    OPT_IQ_CORRECTION, // All other IQ options (period, initial mag/phase) have been removed.
    OPT_DC_BLOCK,
    OPT_PRESET,
};

// --- Forward declarations for helper functions ---
static void print_usage_input_section(int option_width);
static void print_usage_output_destination_section(int option_width);
static void print_usage_output_options_section(int option_width);
static void print_usage_wav_specific_options_section(int option_width);
static void print_usage_raw_file_specific_options_section(int option_width);
static void print_usage_sdr_general_options_section(int option_width);
static void print_usage_sdrplay_specific_options_section(int option_width);
static void print_usage_hackrf_specific_options_section(int option_width);
static void print_usage_processing_options_section(int option_width);
static format_t get_format(const char *name);

// --- Forward declarations for validation functions ---
static bool validate_input_source(AppConfig *config, int argc, char *argv[], int *optind_ptr);
static bool validate_output_destination(AppConfig *config);
static bool validate_output_type_and_sample_format(AppConfig *config);
static bool validate_sdr_specific_options(const AppConfig *config);
static bool validate_wav_specific_options(const AppConfig *config);
static bool validate_raw_file_specific_options(const AppConfig *config);
static bool validate_processing_options(AppConfig *config);
static bool validate_iq_correction_options(AppConfig *config);


// --- Argument Parsing Helper Functions ---

static bool parse_double_arg(const char* optarg, const char* opt_name, double* out_val, bool allow_negative, bool allow_zero) {
    char *endptr;
    errno = 0;
    *out_val = strtod(optarg, &endptr);

    if (errno != 0 || *endptr != '\0' || !isfinite(*out_val)) {
        log_fatal("Invalid numeric value for %s: '%s'. Must be a valid number.", opt_name, optarg);
        return false;
    }
    if (!allow_zero && *out_val == 0.0) {
        log_fatal("Value for %s cannot be zero.", opt_name);
        return false;
    }
    if (!allow_negative && *out_val < 0.0) {
        log_fatal("Value for %s must be a positive number.", opt_name);
        return false;
    }
    return true;
}

static bool parse_float_arg(const char* optarg, const char* opt_name, float* out_val, bool allow_negative, bool allow_zero) {
    double temp_double;
    if (!parse_double_arg(optarg, opt_name, &temp_double, allow_negative, allow_zero)) {
        return false;
    }
    *out_val = (float)temp_double;
    return true;
}

// This function is only needed if SDR support is compiled in.
#if defined(WITH_SDRPLAY) || defined(WITH_HACKRF)
static bool parse_long_arg(const char* optarg, const char* opt_name, long* out_val, bool allow_negative) {
    char *endptr;
    errno = 0;
    *out_val = strtol(optarg, &endptr, 10);

    if (errno != 0 || *endptr != '\0') {
        log_fatal("Invalid integer value for %s: '%s'.", opt_name, optarg);
        return false;
    }
    if (!allow_negative && *out_val < 0) {
        log_fatal("Value for %s must be a non-negative integer.", opt_name);
        return false;
    }
    return true;
}
#endif

// parse_uint32_arg is only used by HackRF options, so it can be conditional
#if defined(WITH_HACKRF)
static bool parse_uint32_arg(const char* optarg, const char* opt_name, uint32_t* out_val) {
    long temp_long;
    if (!parse_long_arg(optarg, opt_name, &temp_long, false)) { // Disallow negative
        return false;
    }
    if (temp_long > UINT32_MAX) {
        log_fatal("Value for %s is too large.", opt_name);
        return false;
    }
    *out_val = (uint32_t)temp_long;
    return true;
}
#endif


/**
 * @brief Prints command usage information to stderr.
 */
void print_usage(const char *prog_name) {
    const int option_width = 38;

    fprintf(stderr, "Usage: %s -i {wav <file_path> | raw-file <file_path> | sdrplay | hackrf} {--file <path> | --stdout} [options]\n\n", prog_name);
    fprintf(stderr, "Description:\n");
    fprintf(stderr, "  Resamples an I/Q file or a stream from an SDR device to a specified format and sample rate.\n\n");

    print_usage_input_section(option_width);
    print_usage_output_destination_section(option_width);
    print_usage_output_options_section(option_width);
    print_usage_wav_specific_options_section(option_width);
    print_usage_raw_file_specific_options_section(option_width);
    print_usage_sdr_general_options_section(option_width);
    print_usage_sdrplay_specific_options_section(option_width);
    print_usage_hackrf_specific_options_section(option_width);
    print_usage_processing_options_section(option_width);
}


// --- Helper functions for print_usage ---

static void print_usage_input_section(int option_width) {
    fprintf(stderr, "Required Input:\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "-i, --input <type>", "Specifies the input type. Must be one of:");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "wav:      Input from a WAV file specified by <file_path>.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "raw-file: Input from a headerless file of raw I/Q samples specified by <file_path>.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "sdrplay:  Input from a SDRplay device.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "hackrf:   Input from a HackRF device.");
    fprintf(stderr, "\n");
}

static void print_usage_output_destination_section(int option_width) {
    fprintf(stderr, "Output Destination (Required, choose one):\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "-f, --file <file>", "Output to a file.");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "-o, --stdout", "Output binary data for piping to another program.");
}

static void print_usage_output_options_section(int option_width) {
    fprintf(stderr, "Output Options:\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "--output-container <type>", "Specifies the output file container format.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Defaults to 'wav-rf64' for file output, 'raw' for stdout.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "raw:      Headerless, raw I/Q sample data.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "wav:      Standard WAV format (max 4GB, limited sample rates).");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", "wav-rf64: RF64/BW64 format for large files and high sample rates.");

    fprintf(stderr, "  %-*s %s\n", option_width, "--output-sample-format <format>", "Sample format for output data. (Defaults to cs16 for file output).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "cs8, cu8, cs16, cu16, cs32, cu32, cf32");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", "(Not all formats are compatible with WAV containers).");
}

static void print_usage_wav_specific_options_section(int option_width) {
    fprintf(stderr, "WAV Input Specific Options (Only valid with '--input wav'):\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "--wav-center-target-frequency <hz>", "Shift signal to a new target center frequency (e.g., 97.3e6).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "(Recommended for WAV captures with frequency metadata).");
    fprintf(stderr, "  %-*s %s\n", option_width, "--wav-shift-frequency <hz>", "Apply a direct frequency shift in Hz.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "(Use if WAV input lacks metadata or for manual correction).");
    fprintf(stderr, "  %-*s %s\n", option_width, "--wav-shift-after-resample", "Apply frequency shift AFTER resampling (default is before).");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", "(A workaround for narrow I/Q WAV recordings where only a single");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", " HD sideband is present).");
}

static void print_usage_raw_file_specific_options_section(int option_width) {
    fprintf(stderr, "Raw File Input Options (Only valid with '--input raw-file'):\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "--raw-file-input-rate <hz>", "(Required) The sample rate of the raw input file.");
    fprintf(stderr, "  %-*s %s\n", option_width, "--raw-file-input-sample-format <format>", "(Required) The sample format of the raw input file.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Valid formats: cs8, cu8, cs16, cu16, cs32, cu32, cf32");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", "(File is assumed to be 2-channel interleaved I/Q data).");
}

static void print_usage_sdr_general_options_section(int option_width) {
    fprintf(stderr, "SDR Options (Only valid when using an SDR input):\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "--rf-freq <hz>", "(Required) Tuner center frequency in Hz (e.g., 97.3e6).");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "--bias-t", "(Optional) Enable Bias-T power.");
}

static void print_usage_sdrplay_specific_options_section(int option_width) {
    fprintf(stderr, "SDRplay-Specific Options (Only valid with '--input sdrplay'):\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "--sdrplay-sample-rate <hz>", "Set sample rate in Hz. (Optional, Default: 2e6).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Valid range: 2e6 to 10e6.");
    fprintf(stderr, "  %-*s %s\n", option_width, "--sdrplay-bandwidth <hz>", "Set analog bandwidth in Hz. (Optional, Default: 1.536e6).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Valid values: 200e3, 300e3, 600e3, 1.536e6, 5e6, 6e6, 7e6, 8e6.");
    fprintf(stderr, "  %-*s %s\n", option_width, "--sdrplay-device-idx <IDX>", "Select specific SDRplay device by index (0-indexed). (Default: 0).");
    fprintf(stderr, "  %-*s %s\n", option_width, "--sdrplay-gain-level <LEVEL>", "Set manual gain level (0=min gain). Disables AGC.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Max level varies by device/freq (e.g., RSP1A: 0-9, RSPdx @100MHz: 0-27).");
    fprintf(stderr, "  %-*s %s\n", option_width, "--sdrplay-antenna <PORT>", "Select antenna port (device-specific).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "RSPdx/R2: A, B, C | RSP2: A, B, HIZ | RSPduo: A, HIZ");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "(Not applicable for RSP1, RSP1A, RSP1B).");
    fprintf(stderr, "  %-*s %s\n", option_width, "--sdrplay-hdr-mode", "(Optional) Enable HDR mode on RSPdx/RSPdxR2.");
    fprintf(stderr, "  %-*s %s\n", option_width, "--sdrplay-hdr-bw <BW_MHZ>", "Set bandwidth for HDR mode. Requires --sdrplay-hdr-mode. (Default: 1.7).");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", "Valid values: 0.2, 0.5, 1.2, 1.7.");
}

static void print_usage_hackrf_specific_options_section(int option_width) {
    fprintf(stderr, "HackRF-Specific Options (Only valid with '--input hackrf'):\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "--hackrf-sample-rate <hz>", "Set sample rate in Hz. (Optional, Default: 8e6).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Valid range is 2-20 Msps (e.g., 2e6, 10e6, 20e6).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Automatically selects a suitable baseband filter.");
    fprintf(stderr, "  %-*s %s\n", option_width, "--hackrf-lna-gain <db>", "Set LNA (IF) gain in dB. (Optional, Default: 16).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Valid values: 0-40 in 8 dB steps (e.g., 0, 8, 16, 24, 32, 40).");
    fprintf(stderr, "  %-*s %s\n", option_width, "--hackrf-vga-gain <db>", "Set VGA (Baseband) gain in dB. (Optional, Default: 0).");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "Valid values: 0-62 in 2 dB steps (e.g., 0, 2, 4, ... 62).");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "--hackrf-amp-enable", "Enable the front-end RF amplifier (+14 dB).");
}

static void print_usage_processing_options_section(int option_width) {
    fprintf(stderr, "Processing Options:\n");
    fprintf(stderr, "  %-*s %s\n", option_width, "--output-rate <hz>", "Output sample rate in Hz. (Required if no preset is used).");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", "(Cannot be used with --preset or --no-resample).");
    fprintf(stderr, "  %-*s %s\n\n", option_width, "--gain <multiplier>", "Apply a linear gain multiplier to the samples (Default: 1.0).");
    fprintf(stderr, "  %-*s %s\n", option_width, "--no-resample", "Disable the resampler (passthrough mode).");
    fprintf(stderr, "  %-*s   %s\n\n", option_width, "", "Output sample rate will be the same as the input rate.");

    // I/Q Correction Options (Simplified)
    fprintf(stderr, "  %-*s %s\n\n", option_width, "--iq-correction", "(Optional) Enable automatic I/Q imbalance correction.");

    // DC Block Option
    fprintf(stderr, "  %-*s %s\n\n", option_width, "--dc-block", "(Optional) Enable DC offset removal (high-pass filter).");

    // Preset option (moved to last)
    fprintf(stderr, "  %-*s %s\n", option_width, "--preset <name>", "Use a preset for a common target.");
    fprintf(stderr, "  %-*s   %s\n", option_width, "", "(Cannot be used with --no-resample).");

    if (g_config.num_presets > 0) {
        fprintf(stderr, "  %-*s   %s\n", option_width, "", "Loaded presets:");
        for (int i = 0; i < g_config.num_presets; i++) {
            char preset_label[128];
            const PresetDefinition* p = &g_config.presets[i];
            snprintf(preset_label, sizeof(preset_label), "%s:", p->name);
            fprintf(stderr, "  %-*s     %-15s %s\n", option_width, "", preset_label, p->description);
        }
    }
}


/**
 * @brief Parses command line arguments using getopt_long.
 */
bool parse_arguments(int argc, char *argv[], AppConfig *config) {
    int opt;
    int long_index = 0;
    const char* short_opts = "i:of:h";

    optind = 1;
    opterr = 1;

    static const struct option long_options[] = {
        {"input",                     required_argument, 0, 'i'},
        {"stdout",                    no_argument,       0, 'o'},
        {"file",                      required_argument, 0, 'f'},
        {"help",                      no_argument,       0, 'h'},
        {"output-container",          required_argument, 0, OPT_OUTPUT_CONTAINER},
        {"output-sample-format",      required_argument, 0, OPT_OUTPUT_SAMPLE_FORMAT},
        {"gain",                      required_argument, 0, OPT_GAIN},
        {"rf-freq",                   required_argument, 0, OPT_SDR_RF_FREQ},
        {"bias-t",                    no_argument,       0, OPT_SDR_BIAS_T},
        {"sdrplay-device-idx",        required_argument, 0, OPT_SDRPLAY_DEVICE_IDX},
        {"sdrplay-gain-level",        required_argument, 0, OPT_SDRPLAY_GAIN_LEVEL},
        {"sdrplay-antenna",           required_argument, 0, OPT_SDRPLAY_ANTENNA},
        {"sdrplay-hdr-mode",          no_argument,       0, OPT_SDRPLAY_HDR_MODE},
        {"sdrplay-hdr-bw",            required_argument, 0, OPT_SDRPLAY_HDR_BW},
        {"sdrplay-sample-rate",       required_argument, 0, OPT_SDRPLAY_SAMPLE_RATE},
        {"sdrplay-bandwidth",         required_argument, 0, OPT_SDRPLAY_BANDWIDTH},
        {"hackrf-lna-gain",           required_argument, 0, OPT_HACKRF_LNA_GAIN},
        {"hackrf-vga-gain",           required_argument, 0, OPT_HACKRF_VGA_GAIN},
        {"hackrf-amp-enable",         no_argument,       0, OPT_HACKRF_AMP_ENABLE},
        {"hackrf-sample-rate",        required_argument, 0, OPT_HACKRF_SAMPLE_RATE},
        {"wav-center-target-frequency", required_argument, 0, OPT_WAV_CENTER_TARGET_FREQUENCY},
        {"wav-shift-frequency",       required_argument, 0, OPT_WAV_SHIFT_FREQUENCY},
        {"wav-shift-after-resample",  no_argument,       0, OPT_WAV_SHIFT_AFTER_RESAMPLE},
        {"raw-file-input-rate",       required_argument, 0, OPT_RAW_FILE_INPUT_RATE},
        {"raw-file-input-sample-format", required_argument, 0, OPT_RAW_FILE_INPUT_SAMPLE_FORMAT},
        {"output-rate",               required_argument, 0, OPT_OUTPUT_RATE},
        {"no-resample",               no_argument,       0, OPT_NO_RESAMPLE},
        {"iq-correction",             no_argument,       0, OPT_IQ_CORRECTION},
        {"dc-block",                  no_argument,       0, OPT_DC_BLOCK},
        {"preset",                    required_argument, 0, OPT_PRESET},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, short_opts, long_options, &long_index)) != -1) {
         switch (opt) {
            case 'i': config->input_type_str = optarg; break;
            case 'o': config->output_to_stdout = true; break;
            case 'f': config->output_filename_arg = optarg; break;
            case 'h':
                config->help_requested = true;
                return true;

            case OPT_GAIN:
                if (config->gain_provided) {
                    log_fatal("Option --gain cannot be specified more than once.");
                    return false;
                }
                if (!parse_float_arg(optarg, "--gain", &config->gain, false, false)) return false;
                config->gain_provided = true;
                break;

            case OPT_WAV_SHIFT_FREQUENCY:
                if (!parse_double_arg(optarg, "--wav-shift-frequency", &config->freq_shift_hz, true, true)) return false;
                config->freq_shift_requested = true;
                break;

            case OPT_WAV_CENTER_TARGET_FREQUENCY:
                if (!parse_double_arg(optarg, "--wav-center-target-frequency", &config->center_frequency_target_hz, true, true)) return false;
                config->set_center_frequency_target_hz = true;
                break;

            case OPT_OUTPUT_SAMPLE_FORMAT:
                config->sample_type_name = optarg;
                break;
            case OPT_WAV_SHIFT_AFTER_RESAMPLE:
                config->shift_after_resample = true;
                break;
            case OPT_OUTPUT_CONTAINER:
                config->output_type_name = optarg;
                config->output_type_provided = true;
                break;

            case OPT_SDR_RF_FREQ:
#if defined(WITH_SDRPLAY) || defined(WITH_HACKRF)
                if (!parse_double_arg(optarg, "--rf-freq", &config->sdr.rf_freq_hz, false, false)) return false;
                config->sdr.rf_freq_provided = true;
#else
                log_warn("Option --rf-freq ignored: No SDR hardware devices enabled in this build.");
#endif
                break;

            case OPT_SDR_BIAS_T:
#if defined(WITH_SDRPLAY) || defined(WITH_HACKRF)
                config->sdr.bias_t_enable = true;
#else
                log_warn("Option --bias-t ignored: No SDR hardware devices enabled in this build.");
#endif
                break;

            case OPT_SDRPLAY_DEVICE_IDX:
#if defined(WITH_SDRPLAY)
                {
                    long val;
                    if (!parse_long_arg(optarg, "--sdrplay-device-idx", &val, false)) return false;
                    config->sdrplay.device_index = (int)val;
                }
#else
                log_warn("Option --sdrplay-device-idx ignored: SDRplay support not enabled in this build.");
#endif
                break;

            case OPT_SDRPLAY_GAIN_LEVEL:
#if defined(WITH_SDRPLAY)
                {
                    long val;
                    if (!parse_long_arg(optarg, "--sdrplay-gain-level", &val, false)) return false;
                    config->sdrplay.gain_level = (int)val;
                    config->sdrplay.gain_level_provided = true;
                }
#else
                log_warn("Option --sdrplay-gain-level ignored: SDRplay support not enabled in this build.");
#endif
                break;

            case OPT_SDRPLAY_ANTENNA:
#if defined(WITH_SDRPLAY)
                config->sdrplay.antenna_port_name = optarg;
#else
                log_warn("Option --sdrplay-antenna ignored: SDRplay support not enabled in this build.");
#endif
                break;

            case OPT_SDRPLAY_HDR_MODE:
#if defined(WITH_SDRPLAY)
                config->sdrplay.use_hdr_mode = true;
#else
                log_warn("Option --sdrplay-hdr-mode ignored: SDRplay support not enabled in this build.");
#endif
                break;

            case OPT_SDRPLAY_HDR_BW:
#if defined(WITH_SDRPLAY)
                {
                    const char* bw_str = optarg;
                    if      (strcmp(bw_str, "0.2") == 0) config->sdrplay.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_0_200;
                    else if (strcmp(bw_str, "0.5") == 0) config->sdrplay.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_0_500;
                    else if (strcmp(bw_str, "1.2") == 0) config->sdrplay.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_1_200;
                    else if (strcmp(bw_str, "1.7") == 0) config->sdrplay.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_1_700;
                    else {
                        log_fatal("Invalid HDR bandwidth '%s'. Valid values are 0.2, 0.5, 1.2, 1.7.", bw_str);
                        return false;
                    }
                    config->sdrplay.hdr_bw_mode_provided = true;
                }
#else
                log_warn("Option --sdrplay-hdr-bw ignored: SDRplay support not enabled in this build.");
#endif
                break;

            case OPT_HACKRF_LNA_GAIN:
#if defined(WITH_HACKRF)
                if (!parse_uint32_arg(optarg, "--hackrf-lna-gain", &config->hackrf.lna_gain)) return false;
                config->hackrf.lna_gain_provided = true;
#else
                log_warn("Option --hackrf-lna-gain ignored: HackRF support not enabled in this build.");
#endif
                break;

            case OPT_HACKRF_VGA_GAIN:
#if defined(WITH_HACKRF)
                if (!parse_uint32_arg(optarg, "--hackrf-vga-gain", &config->hackrf.vga_gain)) return false;
                config->hackrf.vga_gain_provided = true;
#else
                log_warn("Option --hackrf-vga-gain ignored: HackRF support not enabled in this build.");
#endif
                break;

            case OPT_HACKRF_AMP_ENABLE:
#if defined(WITH_HACKRF)
                config->hackrf.amp_enable = true;
#else
                log_warn("Option --hackrf-amp-enable ignored: HackRF support not enabled in this build.");
#endif
                break;

            case OPT_OUTPUT_RATE:
                if (!parse_double_arg(optarg, "--output-rate", &config->user_defined_target_rate, false, false)) return false;
                config->user_rate_provided = true;
                break;

            case OPT_HACKRF_SAMPLE_RATE:
#if defined(WITH_HACKRF)
                if (!parse_double_arg(optarg, "--hackrf-sample-rate", &config->hackrf.sample_rate_hz, false, false)) return false;
                config->hackrf.sample_rate_provided = true;
#else
                log_warn("Option --hackrf-sample-rate ignored: HackRF support not enabled in this build.");
#endif
                break;

            case OPT_SDRPLAY_SAMPLE_RATE:
#if defined(WITH_SDRPLAY)
                if (!parse_double_arg(optarg, "--sdrplay-sample-rate", &config->sdrplay.sample_rate_hz, false, false)) return false;
                config->sdrplay.sample_rate_provided = true;
#else
                log_warn("Option --sdrplay-sample-rate ignored: SDRplay support not enabled in this build.");
#endif
                break;

            case OPT_SDRPLAY_BANDWIDTH:
#if defined(WITH_SDRPLAY)
                if (!parse_double_arg(optarg, "--sdrplay-bandwidth", &config->sdrplay.bandwidth_hz, false, false)) return false;
                config->sdrplay.bandwidth_provided = true;
#else
                log_warn("Option --sdrplay-bandwidth ignored: SDRplay support not enabled in this build.");
#endif
                break;

            case OPT_NO_RESAMPLE:
                config->no_resample = true;
                break;

            case OPT_RAW_FILE_INPUT_RATE:
                if (!parse_double_arg(optarg, "--raw-file-input-rate", &config->raw_file.sample_rate_hz, false, false)) return false;
                config->raw_file.sample_rate_provided = true;
                break;

            case OPT_RAW_FILE_INPUT_SAMPLE_FORMAT:
                config->raw_file.format_str = optarg;
                config->raw_file.format_provided = true;
                break;

            case OPT_IQ_CORRECTION:
                config->iq_correction.enable = true;
                break;

            case OPT_DC_BLOCK:
                config->dc_block.enable = true;
                break;

            case OPT_PRESET:
                config->preset_name = optarg;
                break;

            case '?': // Unknown option or missing argument
                return false;
            default:
                log_fatal("Internal error: Unexpected option processing failure.");
                return false;
        }
    }

    // --- Centralized Logic for Option Resolution and Validation ---
    if (!validate_input_source(config, argc, argv, &optind)) return false;
    if (!validate_output_destination(config)) return false;
    if (!validate_output_type_and_sample_format(config)) return false;
    if (!validate_sdr_specific_options(config)) return false;
    if (!validate_wav_specific_options(config)) return false;
    if (!validate_raw_file_specific_options(config)) return false;
    if (!validate_processing_options(config)) return false;
    if (!validate_iq_correction_options(config)) return false;

    // Check for any leftover, non-option arguments
    if (optind < argc) {
        log_fatal("Unexpected non-option arguments found:");
        for (int i = optind; i < argc; i++) { log_error("  %s", argv[i]); }
        return false;
    }

    return true;
}


// --- Implementations of validation helper functions ---

static bool validate_input_source(AppConfig *config, int argc, char *argv[], int *optind_ptr) {
    if (!config->input_type_str) {
        log_fatal("Missing required argument --input <type>.");
        return false;
    }
    if (strcasecmp(config->input_type_str, "wav") == 0) {
        if (*optind_ptr >= argc) {
            log_fatal("Missing <file_path> source after '--input wav'.");
            return false;
        }
        config->input_filename_arg = argv[(*optind_ptr)++];
    } else if (strcasecmp(config->input_type_str, "raw-file") == 0) {
        if (*optind_ptr >= argc) {
            log_fatal("Missing <file_path> source after '--input raw-file'.");
            return false;
        }
        config->input_filename_arg = argv[(*optind_ptr)++];
        if (!config->raw_file.sample_rate_provided) {
            log_fatal("Missing required option --raw-file-input-rate <hz> for raw file input.");
            return false;
        }
        if (!config->raw_file.format_provided) {
            log_fatal("Missing required option --raw-file-input-sample-format <format> for raw file input.");
            return false;
        }
    } else if (strcasecmp(config->input_type_str, "sdrplay") == 0) {
#if defined(WITH_SDRPLAY)
        config->sdrplay.use_sdrplay_input = true;
#else
        log_fatal("Input type 'sdrplay' is not supported in this build. "
                  "Please re-compile with SDRplay support enabled.");
        return false;
#endif
    } else if (strcasecmp(config->input_type_str, "hackrf") == 0) {
#if defined(WITH_HACKRF)
        config->hackrf.use_hackrf_input = true;
#else
        log_fatal("Input type 'hackrf' is not supported in this build. "
                  "Please re-compile with HackRF support enabled.");
        return false;
#endif
    } else {
        log_fatal("Invalid input type '%s'. Must be 'wav', 'raw-file', 'sdrplay', or 'hackrf'.", config->input_type_str);
        return false;
    }
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
    // --- Step 1: Resolve preset if provided ---
    if (config->preset_name) {
        bool preset_found = false;
        for (int i = 0; i < config->num_presets; i++) {
            if (strcasecmp(config->preset_name, config->presets[i].name) == 0) {
                const PresetDefinition* p = &config->presets[i];
                config->target_rate = p->target_rate;

                // Preset provides defaults ONLY if not specified by the user
                if (!config->sample_type_name) {
                    config->sample_type_name = p->sample_format_name;
                }
                if (!config->output_type_provided) {
                    config->output_type = p->output_type;
                    config->output_type_provided = true;
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

    // --- Step 2: Resolve output container type ---
    if (!config->output_type_provided) {
        // Set default based on destination
        config->output_type = config->output_to_stdout ? OUTPUT_TYPE_RAW : OUTPUT_TYPE_WAV_RF64;
    } else if (config->output_type_name) {
        // User provided a name, so parse it
        if (strcasecmp(config->output_type_name, "raw") == 0) config->output_type = OUTPUT_TYPE_RAW;
        else if (strcasecmp(config->output_type_name, "wav") == 0) config->output_type = OUTPUT_TYPE_WAV;
        else if (strcasecmp(config->output_type_name, "wav-rf64") == 0) config->output_type = OUTPUT_TYPE_WAV_RF64;
        else {
            log_fatal("Invalid output type '%s'. Must be 'raw', 'wav', or 'wav-rf64'.", config->output_type_name);
            return false;
        }
    }

    // --- Step 3: Set target rate if user provided it directly ---
    if (config->user_rate_provided) {
        config->target_rate = config->user_defined_target_rate;
    }

    // --- Step 4: Resolve output sample format ---
    if (!config->sample_type_name) {
        // If still no sample format, set default for file output
        if (config->output_filename_arg && !config->output_to_stdout) {
            config->sample_type_name = "cs16";
        } else {
            log_fatal("Missing required argument: you must specify an --output-sample-format or use a preset.");
            return false;
        }
    }

    // --- Step 5: Parse sample format name ---
    config->output_format = get_format(config->sample_type_name);
    if (config->output_format == FORMAT_UNKNOWN) {
        log_fatal("Invalid sample format '%s'. See --help for valid formats.", config->sample_type_name);
        return false;
    }

    // --- Step 6: Final validation checks for combinations ---
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

static bool validate_sdr_specific_options(const AppConfig *config) {
    bool is_file_input = (config->input_type_str &&
                          (strcasecmp(config->input_type_str, "wav") == 0 ||
                           strcasecmp(config->input_type_str, "raw-file") == 0));
    if (is_file_input) {
#if defined(WITH_SDRPLAY) || defined(WITH_HACKRF)
        if (config->sdr.rf_freq_provided) {
            log_fatal("Option --rf-freq is not valid for file-based inputs ('wav', 'raw-file').");
            return false;
        }
        if (config->sdr.bias_t_enable) {
            log_fatal("Option --bias-t is not valid for file-based inputs ('wav', 'raw-file').");
            return false;
        }
#endif
    }
    return true;
}

static bool validate_wav_specific_options(const AppConfig *config) {
    bool wav_option_used = config->set_center_frequency_target_hz || config->freq_shift_requested || config->shift_after_resample;
    bool is_wav_input = (config->input_type_str && strcasecmp(config->input_type_str, "wav") == 0);

    if (wav_option_used && !is_wav_input) {
        log_fatal("Options --wav-center-target-frequency, --wav-shift-frequency, and --wav-shift-after-resample are only valid for 'wav' input.");
        return false;
    }

    if (config->shift_after_resample) {
        if (!config->freq_shift_requested && !config->set_center_frequency_target_hz) {
            log_fatal("Option --wav-shift-after-resample specified, but no frequency shift was requested.");
            return false;
        }
    }
    return true;
}

static bool validate_raw_file_specific_options(const AppConfig *config) {
    bool raw_file_option_used = config->raw_file.sample_rate_provided || config->raw_file.format_provided;
    bool is_raw_file_input = (config->input_type_str && strcasecmp(config->input_type_str, "raw-file") == 0);

    if (raw_file_option_used && !is_raw_file_input) {
        log_fatal("Options --raw-file-input-rate and --raw-file-input-sample-format are only valid for 'raw-file' input.");
        return false;
    }
    return true;
}

static bool validate_processing_options(AppConfig *config) {
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

    if (config->target_rate <= 0 && !config->no_resample) {
        log_fatal("Missing required argument: you must specify an --output-rate or use a preset.");
        return false;
    }

    return true;
}

static bool validate_iq_correction_options(AppConfig *config) {
    // If I/Q correction is enabled, ensure DC block is also enabled.
    if (config->iq_correction.enable) {
        if (!config->dc_block.enable) {
            log_fatal("Option --iq-correction requires --dc-block to be enabled for optimal performance and stability.");
            return false;
        }
    }
    return true;
}


/**
 * @brief Parses a format string name into a format_t enum.
 */
static format_t get_format(const char *name) {
    if (strcasecmp(name, "s8") == 0) return S8;
    if (strcasecmp(name, "u8") == 0) return U8;
    if (strcasecmp(name, "s16") == 0) return S16;
    if (strcasecmp(name, "u16") == 0) return U16;
    if (strcasecmp(name, "s32") == 0) return S32;
    if (strcasecmp(name, "u32") == 0) return U32;
    if (strcasecmp(name, "f32") == 0) return F32;
    if (strcasecmp(name, "cs8") == 0) return CS8;
    if (strcasecmp(name, "cu8") == 0) return CU8;
    if (strcasecmp(name, "cs16") == 0) return CS16;
    if (strcasecmp(name, "cu16") == 0) return CU16;
    if (strcasecmp(name, "cs32") == 0) return CS32;
    if (strcasecmp(name, "cu32") == 0) return CU32;
    if (strcasecmp(name, "cf32") == 0) return CF32;
    return FORMAT_UNKNOWN;
}
