#include "config.h"
#include "app_context.h" // Provides the full definition for AppConfig
#include "constants.h"
#include "log.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/**
 * @brief Parses a string in the format "start:end" into two float values.
 * @return true on success, false on parsing failure.
 */
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

/**
 * @brief Adds a new filter request to the configuration's filter chain.
 */
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

// RESTRUCTURED: This function has been reordered to check for fatal errors first
// and to explicitly log the application's "smart default" behaviors.
bool validate_output_type_and_sample_format(AppConfig *config) {
    // --- Step 1: Process Presets (Highest Priority) ---
    if (config->preset_name) {
        bool preset_found = false;
        for (int i = 0; i < config->num_presets; i++) {
            if (strcasecmp(config->preset_name, config->presets[i].name) == 0) {
                const PresetDefinition* p = &config->presets[i];
                config->target_rate = p->target_rate;
                if (!config->output_sample_format_name) {
                    config->output_sample_format_name = p->output_sample_format_name;
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
                if (p->lowpass_cutoff_hz_provided && config->lowpass_cutoff_hz_arg[0] == 0.0f) {
                    config->lowpass_cutoff_hz_arg[0] = p->lowpass_cutoff_hz;
                }
                if (p->highpass_cutoff_hz_provided && config->highpass_cutoff_hz_arg[0] == 0.0f) {
                    config->highpass_cutoff_hz_arg[0] = p->highpass_cutoff_hz;
                }
                if (p->pass_range_str_provided && !config->pass_range_str_arg[0]) {
                    config->pass_range_str_arg[0] = p->pass_range_str;
                }
                if (p->stopband_str_provided && !config->stopband_str_arg[0]) {
                    config->stopband_str_arg[0] = p->stopband_str;
                }
                if (p->transition_width_hz_provided && config->transition_width_hz_arg == 0.0f) {
                    config->transition_width_hz_arg = p->transition_width_hz;
                }
                if (p->filter_taps_provided && config->filter_taps_arg == 0) {
                    config->filter_taps_arg = p->filter_taps;
                }
                if (p->attenuation_db_provided && config->attenuation_db_arg == 0.0f) {
                    config->attenuation_db_arg = p->attenuation_db;
                }
                if (p->filter_type_str_provided && !config->filter_type_str_arg) {
                    config->filter_type_str_arg = p->filter_type_str;
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

    // --- Step 2: Process Explicit User Rate ---
    if (config->user_defined_target_rate_arg > 0.0f) {
        config->target_rate = (double)config->user_defined_target_rate_arg;
        config->user_rate_provided = true;
    }

    // --- Step 3: FATAL CHECK for Missing Rate (MOVED HERE) ---
    if (config->target_rate <= 0 && !config->no_resample) {
        log_fatal("Missing required argument: you must specify an --output-rate or use a preset.");
        return false;
    }

    // --- Step 4: Determine Output Container Type (with defaults) ---
    // This logic is now simplified as the module choice implies the container.
    if (strcasecmp(config->output_module_str, "raw") == 0) {
        config->output_type = OUTPUT_TYPE_RAW;
    } else if (strcasecmp(config->output_module_str, "wav") == 0) {
        // For file output, default to WAV_RF64 but make it explicit to the user.
        config->output_type = OUTPUT_TYPE_WAV_RF64;
        log_info("Defaulting to 'wav-rf64' container for large file support.");
    } else if (strcasecmp(config->output_module_str, "stdout") == 0) {
        config->output_type = OUTPUT_TYPE_RAW;
    }

    // --- Step 5: Determine Output Sample Format (with defaults) ---
    if (!config->output_sample_format_name) {
        // If writing to a file, and no sample format is given,
        // it's safe to default to 'cs16', which is the most common for WAV files.
        if (config->output_filename_arg) {
            config->output_sample_format_name = "cs16";
            log_info("No output sample format specified; defaulting to 'cs16' for file output.");
        } else {
            // For stdout, the format MUST be specified as we cannot guess the consumer's needs.
            log_fatal("Missing required argument: you must specify an --output-sample-format when using '--output stdout'.");
            return false;
        }
    }

    // --- Step 6: Final Validation of Formats and Combinations ---
    config->output_format = utils_get_format_from_string(config->output_sample_format_name);
    if (config->output_format == FORMAT_UNKNOWN) {
        log_fatal("Invalid sample format '%s'. See --help for valid formats.", config->output_sample_format_name);
        return false;
    }

    if (config->output_type == OUTPUT_TYPE_WAV || config->output_type == OUTPUT_TYPE_WAV_RF64) {
        if (config->output_format != CS16 && config->output_format != CU8) {
            log_fatal("Invalid sample format '%s' for WAV container. Only 'cs16' and 'cu8' are supported for WAV output.", config->output_sample_format_name);
            return false;
        }
    }

    return true;
}

bool validate_filter_options(AppConfig *config) {
    config->num_filter_requests = 0;

    for (int i = 0; i < MAX_FILTER_CHAIN; i++) {
        if (config->lowpass_cutoff_hz_arg[i] > 0.0f) {
            add_filter_request(config, FILTER_TYPE_LOWPASS, config->lowpass_cutoff_hz_arg[i], 0.0f);
        }
        if (config->highpass_cutoff_hz_arg[i] > 0.0f) {
            add_filter_request(config, FILTER_TYPE_HIGHPASS, config->highpass_cutoff_hz_arg[i], 0.0f);
        }
        if (config->pass_range_str_arg[i]) {
            float start_f, end_f;
            if (!parse_start_end_string(config->pass_range_str_arg[i], "--pass-range", &start_f, &end_f)) return false;
            float bandwidth = end_f - start_f;
            float center_freq = start_f + (bandwidth / 2.0f);
            add_filter_request(config, FILTER_TYPE_PASSBAND, center_freq, bandwidth);
        }
        if (config->stopband_str_arg[i]) {
            float start_f, end_f;
            if (!parse_start_end_string(config->stopband_str_arg[i], "--stopband", &start_f, &end_f)) return false;
            float bandwidth = end_f - start_f;
            float center_freq = start_f + (bandwidth / 2.0f);
            add_filter_request(config, FILTER_TYPE_STOPBAND, center_freq, bandwidth);
        }
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

bool validate_iq_correction_options(AppConfig *config) {
    if (config->iq_correction.enable) {
        if (!config->dc_block.enable) {
            log_fatal("Option --iq-correction requires --dc-block to be enabled for optimal performance and stability.");
            return false;
        }
    }
    return true;
}

bool validate_option_combinations(AppConfig *config) {
    // --- Validate Filter Implementation Options ---
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
        if (config->filter_type_str_arg && config->filter_type_request == FILTER_TYPE_FIR) {
            log_fatal("Contradictory options: --filter-fft-size cannot be used with an explicit '--filter-type fir'.");
            return false;
        }
 
        if (config->filter_type_request != FILTER_TYPE_FFT) {
            log_debug("Option --filter-fft-size overrides preset; forcing filter type to FFT.");
            config->filter_type_request = FILTER_TYPE_FFT;
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

    if (config->filter_type_request == FILTER_TYPE_FFT && config->filter_taps_arg > 0 && config->filter_fft_size_arg > 0) {
        long adjusted_taps = (config->filter_taps_arg % 2 == 0) 
                           ? config->filter_taps_arg + 1 
                           : config->filter_taps_arg;
        long required_fft_size = (adjusted_taps - 1) * 2;
        if ((long)config->filter_fft_size_arg < required_fft_size) {
            log_fatal("Parameter conflict: --filter-fft-size (%d) is too small for --filter-taps (%d).",
                      config->filter_fft_size_arg, config->filter_taps_arg);
            log_error("For %ld taps, the FFT size must be at least %ld.",
                      adjusted_taps, required_fft_size);
            return false;
        }
    }

    // --- Validate Conflicting High-Level Modes ---
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
        if (config->freq_shift_hz_arg != 0.0f) {
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

    return true;
}
