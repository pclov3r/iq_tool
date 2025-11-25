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
    if (config->dsp.filter.count < MAX_FILTER_CHAIN) {
        config->dsp.filter.requests[config->dsp.filter.count].type = type;
        config->dsp.filter.requests[config->dsp.filter.count].freq1_hz = f1;
        config->dsp.filter.requests[config->dsp.filter.count].freq2_hz = f2;
        config->dsp.filter.count++;
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
                config->output_rate.target_rate = p->target_rate;
                if (!config->output.format_name) {
                    config->output.format_name = p->output_sample_format_name;
                }
                
                // UPDATED: Map input_gain and output_gain from preset
                if (p->input_gain_provided && config->dsp.input_gain == 1.0f) {
                    config->dsp.input_gain = p->input_gain;
                }
                if (p->output_gain_provided && config->dsp.output_gain == 1.0f) {
                    config->dsp.output_gain = p->output_gain;
                }

                if (p->dc_block_provided && !config->dsp.dc_block.enable) {
                    config->dsp.dc_block.enable = p->dc_block_enable;
                }
                if (p->iq_correction_provided && !config->dsp.iq_correction.enable) {
                    config->dsp.iq_correction.enable = p->iq_correction_enable;
                }

                // --- NEW: Apply AGC Settings from Preset ---
                // If a profile is provided in the preset, it implicitly enables AGC.
                if (p->agc_profile_provided) {
                    if (!config->dsp.agc.enable) {
                        config->dsp.agc.enable = true;
                    }
                    if (!config->dsp.agc.profile_str_arg) {
                        config->dsp.agc.profile_str_arg = p->agc_profile_str;
                    }
                }
                if (p->agc_target_provided && config->dsp.agc.target_level_arg == 0.0f) {
                    config->dsp.agc.target_level_arg = p->agc_target;
                }
                // -------------------------------------------

                if (p->lowpass_cutoff_hz_provided && config->dsp.filter.args.lowpass[0] == 0.0f) {
                    config->dsp.filter.args.lowpass[0] = p->lowpass_cutoff_hz;
                }
                if (p->highpass_cutoff_hz_provided && config->dsp.filter.args.highpass[0] == 0.0f) {
                    config->dsp.filter.args.highpass[0] = p->highpass_cutoff_hz;
                }
                if (p->pass_range_str_provided && !config->dsp.filter.args.pass_range[0]) {
                    config->dsp.filter.args.pass_range[0] = p->pass_range_str;
                }
                if (p->stopband_str_provided && !config->dsp.filter.args.stopband[0]) {
                    config->dsp.filter.args.stopband[0] = p->stopband_str;
                }
                if (p->transition_width_hz_provided && config->dsp.filter.args.transition_width == 0.0f) {
                    config->dsp.filter.args.transition_width = p->transition_width_hz;
                }
                if (p->filter_taps_provided && config->dsp.filter.args.taps == 0) {
                    config->dsp.filter.args.taps = p->filter_taps;
                }
                if (p->attenuation_db_provided && config->dsp.filter.args.attenuation == 0.0f) {
                    config->dsp.filter.args.attenuation = p->attenuation_db;
                }
                if (p->filter_type_str_provided && !config->dsp.filter.args.type_str) {
                    config->dsp.filter.args.type_str = p->filter_type_str;
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
    if (config->output_rate.user_arg > 0.0f) {
        config->output_rate.target_rate = (double)config->output_rate.user_arg;
        config->output_rate.provided = true;
    }

    // --- Step 3: FATAL CHECK for Missing Rate (MOVED HERE) ---
    if (config->output_rate.target_rate <= 0 && !config->dsp.no_resample) {
        log_fatal("Missing required argument: you must specify an --output-rate or use a preset.");
        return false;
    }

    // --- Step 4: Determine Output Container Type (with defaults) ---
    // This logic is now simplified as the module choice implies the container.
    if (strcasecmp(config->output.module_name, "raw") == 0) {
        config->output.type = OUTPUT_TYPE_RAW;
    } else if (strcasecmp(config->output.module_name, "wav") == 0) {
        // For file output, default to WAV_RF64 but make it explicit to the user.
        config->output.type = OUTPUT_TYPE_WAV_RF64;
        log_info("Defaulting to 'wav-rf64' container for large file support.");
    } else if (strcasecmp(config->output.module_name, "stdout") == 0) {
        config->output.type = OUTPUT_TYPE_RAW;
    }

    // --- Step 5: Determine Output Sample Format (with defaults) ---
    if (!config->output.format_name) {
        // If writing to a file, and no sample format is given,
        // it's safe to default to 'cs16', which is the most common for WAV files.
        if (config->output.path_arg) {
            config->output.format_name = "cs16";
            log_info("No output sample format specified; defaulting to 'cs16' for file output.");
        } else {
            // For stdout, the format MUST be specified as we cannot guess the consumer's needs.
            log_fatal("Missing required argument: you must specify an --output-sample-format when using '--output stdout'.");
            return false;
        }
    }

    // --- Step 6: Final Validation of Formats and Combinations ---
    config->output.format = utils_get_format_from_string(config->output.format_name);
    if (config->output.format == FORMAT_UNKNOWN) {
        log_fatal("Invalid sample format '%s'. See --help for valid formats.", config->output.format_name);
        return false;
    }

    if (config->output.type == OUTPUT_TYPE_WAV || config->output.type == OUTPUT_TYPE_WAV_RF64) {
        if (config->output.format != CS16 && config->output.format != CU8) {
            log_fatal("Invalid sample format '%s' for WAV container. Only 'cs16' and 'cu8' are supported for WAV output.", config->output.format_name);
            return false;
        }
    }

    return true;
}

bool validate_filter_options(AppConfig *config) {
    config->dsp.filter.count = 0;

    for (int i = 0; i < MAX_FILTER_CHAIN; i++) {
        if (config->dsp.filter.args.lowpass[i] > 0.0f) {
            add_filter_request(config, FILTER_TYPE_LOWPASS, config->dsp.filter.args.lowpass[i], 0.0f);
        }
        if (config->dsp.filter.args.highpass[i] > 0.0f) {
            add_filter_request(config, FILTER_TYPE_HIGHPASS, config->dsp.filter.args.highpass[i], 0.0f);
        }
        if (config->dsp.filter.args.pass_range[i]) {
            float start_f, end_f;
            if (!parse_start_end_string(config->dsp.filter.args.pass_range[i], "--pass-range", &start_f, &end_f)) return false;
            float bandwidth = end_f - start_f;
            float center_freq = start_f + (bandwidth / 2.0f);
            add_filter_request(config, FILTER_TYPE_PASSBAND, center_freq, bandwidth);
        }
        if (config->dsp.filter.args.stopband[i]) {
            float start_f, end_f;
            if (!parse_start_end_string(config->dsp.filter.args.stopband[i], "--stopband", &start_f, &end_f)) return false;
            float bandwidth = end_f - start_f;
            float center_freq = start_f + (bandwidth / 2.0f);
            add_filter_request(config, FILTER_TYPE_STOPBAND, center_freq, bandwidth);
        }
    }

    if (config->dsp.filter.args.transition_width > 0.0f && config->dsp.filter.args.taps > 0) {
        log_fatal("Error: Cannot specify both --transition-width and --filter-taps at the same time.");
        log_error("Please choose only one method to define the filter's quality.");
        return false;
    }

    if (config->dsp.filter.args.transition_width < 0.0f) {
        log_fatal("--transition-width must be a positive value.");
        return false;
    }

    if (config->dsp.filter.args.taps != 0 && config->dsp.filter.args.taps < 3) {
        log_fatal("--filter-taps must be 3 or greater.");
        return false;
    }
    if (config->dsp.filter.args.taps != 0 && config->dsp.filter.args.taps % 2 == 0) {
        log_warn("--filter-taps must be an odd number. Adjusting from %d to %d.", config->dsp.filter.args.taps, config->dsp.filter.args.taps + 1);
        config->dsp.filter.args.taps++;
    }

    if (config->dsp.filter.args.attenuation <= 0.0f && config->dsp.filter.args.attenuation != 0.0f) {
        log_fatal("--attenuation must be a positive value.");
        return false;
    }

    return true;
}

bool validate_iq_correction_options(AppConfig *config) {
    if (config->dsp.iq_correction.enable) {
        if (!config->dsp.dc_block.enable) {
            log_fatal("Option --iq-correction requires --dc-block to be enabled for optimal performance and stability.");
            return false;
        }
    }
    return true;
}

bool validate_option_combinations(AppConfig *config) {
    // --- Validate Filter Implementation Options ---
    if (config->dsp.filter.args.type_str) {
        if (strcasecmp(config->dsp.filter.args.type_str, "fir") == 0) {
            config->dsp.filter.type_req = FILTER_TYPE_FIR;
        } else if (strcasecmp(config->dsp.filter.args.type_str, "fft") == 0) {
            config->dsp.filter.type_req = FILTER_TYPE_FFT;
        } else {
            log_fatal("Invalid value for --filter-type: '%s'. Must be 'fir' or 'fft'.", config->dsp.filter.args.type_str);
            return false;
        }
    }

    if (config->dsp.filter.args.fft_size != 0) {
        if (config->dsp.filter.args.type_str && config->dsp.filter.type_req == FILTER_TYPE_FIR) {
            log_fatal("Contradictory options: --filter-fft-size cannot be used with an explicit '--filter-type fir'.");
            return false;
        }
 
        if (config->dsp.filter.type_req != FILTER_TYPE_FFT) {
            log_debug("Option --filter-fft-size overrides preset; forcing filter type to FFT.");
            config->dsp.filter.type_req = FILTER_TYPE_FFT;
        }

        if (config->dsp.filter.args.fft_size <= 0) {
            log_fatal("--filter-fft-size must be a positive integer.");
            return false;
        }
        int n = config->dsp.filter.args.fft_size;
        if ((n > 0) && ((n & (n - 1)) != 0)) {
            log_fatal("--filter-fft-size must be a power of two (e.g., 1024, 2048, 4096).");
            return false;
        }
    }

    if (config->dsp.filter.type_req == FILTER_TYPE_FFT && config->dsp.filter.args.taps > 0 && config->dsp.filter.args.fft_size > 0) {
        long adjusted_taps = (config->dsp.filter.args.taps % 2 == 0) 
                           ? config->dsp.filter.args.taps + 1 
                           : config->dsp.filter.args.taps;
        long required_fft_size = (adjusted_taps - 1) * 2;
        if ((long)config->dsp.filter.args.fft_size < required_fft_size) {
            log_fatal("Parameter conflict: --filter-fft-size (%d) is too small for --filter-taps (%d).",
                      config->dsp.filter.args.fft_size, config->dsp.filter.args.taps);
            log_error("For %ld taps, the FFT size must be at least %ld.",
                      adjusted_taps, required_fft_size);
            return false;
        }
    }

    // --- Validate Output AGC Options ---
    if (config->dsp.agc.enable) {
        // 1. Validate Profile
        if (!config->dsp.agc.profile_str_arg) {
            config->dsp.agc.profile = AGC_PROFILE_LOCAL; // Default
        } else {
            if (strcasecmp(config->dsp.agc.profile_str_arg, "dx") == 0) {
                config->dsp.agc.profile = AGC_PROFILE_DX;
            } else if (strcasecmp(config->dsp.agc.profile_str_arg, "local") == 0) {
                config->dsp.agc.profile = AGC_PROFILE_LOCAL;
            } else if (strcasecmp(config->dsp.agc.profile_str_arg, "digital") == 0) {
                config->dsp.agc.profile = AGC_PROFILE_DIGITAL;
            } else {
                log_fatal("Invalid AGC profile '%s'. Must be 'dx', 'local', or 'digital'.", config->dsp.agc.profile_str_arg);
                return false;
            }
        }

        // 2. Validate Target Level
        if (config->dsp.agc.target_level_arg != 0.0f) {
            if (config->dsp.agc.target_level_arg <= 0.0f || config->dsp.agc.target_level_arg > 1.0f) {
                log_fatal("Invalid AGC target level %.2f. Must be between 0.0 and 1.0.", config->dsp.agc.target_level_arg);
                return false;
            }
            config->dsp.agc.target_level = config->dsp.agc.target_level_arg;
        } else {
            // Select default based on profile
            switch (config->dsp.agc.profile) {
                case AGC_PROFILE_DIGITAL:
                    config->dsp.agc.target_level = AGC_DIGITAL_PEAK_TARGET;
                    break;
                case AGC_PROFILE_LOCAL:
                    config->dsp.agc.target_level = AGC_LOCAL_TARGET;
                    break;
                case AGC_PROFILE_DX:
                default:
                    config->dsp.agc.target_level = AGC_DX_TARGET;
                    break;
            }
        }

        // 3. Check for Conflicts
        if (config->dsp.raw_passthrough) {
            log_fatal("Option --output-agc cannot be used with --raw-passthrough.");
            return false;
        }

        // NEW: Conflict check for Output Gain
        if (config->dsp.output_gain != 1.0f) {
            log_fatal("Conflicting options: --output-agc and --output-gain-multiplier cannot be used together.");
            return false;
        }

        // 4. Check for Warnings (Updated variable name)
        if (config->dsp.input_gain_provided && config->dsp.input_gain != 1.0f) {
            log_warn("Both --input-gain-multiplier and --output-agc are set.");
            log_warn("Manual gain is applied at input, but AGC will override the final volume at output.");
        }
    }

    // --- Validate Conflicting High-Level Modes ---
    if (config->output_rate.provided && config->preset_name) {
        log_fatal("Option --output-rate cannot be used with --preset.");
        return false;
    }
    if (config->dsp.no_resample) {
        if (config->output_rate.provided) {
            log_fatal("Option --no-resample cannot be used with --output-rate.");
            return false;
        }
        if (config->preset_name) {
            log_fatal("Option --no-resample cannot be used with --preset.");
            return false;
        }
    }

    if (config->dsp.raw_passthrough) {
        if (config->dsp.filter.count > 0) {
            log_fatal("Option --raw-passthrough cannot be used with any filtering options.");
            return false;
        }
        if (!config->dsp.no_resample) {
            log_warn("Option --raw-passthrough implies --no-resample. Forcing resampler off.");
            config->dsp.no_resample = true;
        }
        if (config->dsp.freq_shift_hz != 0.0f) {
            log_fatal("Option --raw-passthrough cannot be used with frequency shifting options.");
            return false;
        }
        if (config->dsp.iq_correction.enable) {
            log_fatal("Option --raw-passthrough cannot be used with --iq-correction.");
            return false;
        }
        if (config->dsp.dc_block.enable) {
            log_fatal("Option --raw-passthrough cannot be used with --dc-block.");
            return false;
        }
    }

    return true;
}
