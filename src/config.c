#include "config.h"
#include "app_context.h"
#include "constants.h"
#include "log.h"
#include "module_registry.h"
#include "utils.h"
#include "sample_format_table.h"
#include "agc.h"
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

static bool parse_start_end_string(const char* input_str, const char* arg_name, float* out_start, float* out_end) {
    char start_buf[128], end_buf[128];
    if (sscanf(input_str, "%127[^:]:%127s", start_buf, end_buf) != 2) {
        log_error("Invalid format for %s. Expected 'start_freq:end_freq'. Found '%s'.", arg_name, input_str);
        return false;
    }
    char* endptr1; char* endptr2;
    *out_start = strtof(start_buf, &endptr1);
    *out_end = strtof(end_buf, &endptr2);
    if (*endptr1 != '\0' || *endptr2 != '\0') {
        log_fatal("Invalid numerical value in %s argument. Could not parse '%s'.", arg_name, input_str);
        return false;
    }
    if (*out_end <= *out_start) {
        log_error("In %s argument, end frequency must be greater than start frequency.", arg_name);
        return false;
    }
    return true;
}

static void add_filter_request(AppConfig *config, FilterType type, float f1, float f2) {
    if (config->dsp.filter.count < MAX_FILTER_CHAIN) {
        config->dsp.filter.requests[config->dsp.filter.count] = (FilterRequest){ .type = type, .freq1_hz = f1, .freq2_hz = f2 };
        config->dsp.filter.count++;
    } else {
        log_warn("Maximum number of chained filters (%d) reached. Ignoring further filter options.", MAX_FILTER_CHAIN);
    }
}

bool validate_output_type_and_sample_format(AppConfig *config) {
    const Module* out_mod = module_get(config->output.module_name, MODULE_TYPE_OUTPUT, NULL);
    if (out_mod) {
        config->output.payload = out_mod->payload;
    }

    if (!config->output.sample_format_str) {
        if (config->output.path_arg) {
            config->output.sample_format_str = "cs16";
            log_info("No output sample format specified; defaulting to 'cs16' for file output.");
        } else if (out_mod && out_mod->module_defines_format) {
            config->output.sample_format_str = "cs16";
        } else {
            log_error("Missing required argument: --output-sample-format for output '%s'.", config->output.module_name);
            return false;
        }
    }

    config->output.sample_format = get_format_info_by_name(config->output.sample_format_str) ? get_format_info_by_name(config->output.sample_format_str)->format_enum : FORMAT_UNKNOWN;
    if (config->output.sample_format == FORMAT_UNKNOWN) {
        log_error("Invalid sample format '%s'.", config->output.sample_format_str);
        return false;
    }
    return true;
}

bool validate_filter_options(AppConfig *config) {
    config->dsp.filter.count = 0;
    for (int i = 0; i < MAX_FILTER_CHAIN; i++) {
        if (config->dsp.filter.args.lowpass[i] > 0.0f) add_filter_request(config, FILTER_TYPE_LOWPASS, config->dsp.filter.args.lowpass[i], 0.0f);
        if (config->dsp.filter.args.highpass[i] > 0.0f) add_filter_request(config, FILTER_TYPE_HIGHPASS, config->dsp.filter.args.highpass[i], 0.0f);
        if (config->dsp.filter.args.pass_range[i]) {
            float start_f, end_f;
            if (!parse_start_end_string(config->dsp.filter.args.pass_range[i], "--pass-range", &start_f, &end_f)) return false;
            add_filter_request(config, FILTER_TYPE_PASSBAND, start_f + ((end_f - start_f) / 2.0f), end_f - start_f);
        }
        if (config->dsp.filter.args.stopband[i]) {
            float start_f, end_f;
            if (!parse_start_end_string(config->dsp.filter.args.stopband[i], "--stopband", &start_f, &end_f)) return false;
            add_filter_request(config, FILTER_TYPE_STOPBAND, start_f + ((end_f - start_f) / 2.0f), end_f - start_f);
        }
    }

    if (config->dsp.filter.args.transition_width > 0.0f && config->dsp.filter.args.taps > 0) {
        log_error("Cannot specify both --transition-width and --filter-taps.");
        return false;
    }
    if (config->dsp.filter.args.taps != 0 && config->dsp.filter.args.taps < 3) {
        log_error("--filter-taps must be 3 or greater.");
        return false;
    }
    if (config->dsp.filter.args.taps != 0 && config->dsp.filter.args.taps % 2 == 0) {
        log_warn("--filter-taps must be odd. Adjusting from %d to %d.", config->dsp.filter.args.taps, config->dsp.filter.args.taps + 1);
        config->dsp.filter.args.taps++;
    }
    if (config->dsp.filter.args.attenuation <= 0.0f && config->dsp.filter.args.attenuation != 0.0f) {
        log_error("--attenuation must be a positive value.");
        return false;
    }
    if (config->dsp.filter.args.attenuation == 0.0f) {
        float resolved_attenuation = 60.0f;
        const Module* in_mod = module_get(config->input.type_name, MODULE_TYPE_INPUT, NULL);
        if (in_mod) {
            resolved_attenuation = (in_mod->default_filter_attenuation_db > 0.0f) ? in_mod->default_filter_attenuation_db :
                (get_format_info_by_enum(config->output.sample_format) ? get_format_info_by_enum(config->output.sample_format)->default_filter_attenuation_db : 60.0f);
        }
        config->dsp.filter.args.attenuation = resolved_attenuation;
    }
    return true;
}

bool validate_iq_correction_options(AppConfig *config) {
    if (config->dsp.iq_correction.enable && !config->dsp.dc_block.enable) {
        log_error("--iq-correction requires --dc-block to be enabled for optimal performance.");
        return false;
    }
    return true;
}

bool validate_option_combinations(AppConfig *config) {
    if (config->dsp.filter.args.type_str) {
        if (strcasecmp(config->dsp.filter.args.type_str, "fir") == 0) config->dsp.filter.type_req = FILTER_TYPE_FIR;
        else if (strcasecmp(config->dsp.filter.args.type_str, "fft") == 0) config->dsp.filter.type_req = FILTER_TYPE_FFT;
        else {
            log_error("Invalid value for --filter-type: '%s'. Must be 'fir' or 'fft'.", config->dsp.filter.args.type_str);
            return false;
        }
    }
    if (config->dsp.filter.args.fft_size != 0) {
        if (config->dsp.filter.args.type_str && config->dsp.filter.type_req == FILTER_TYPE_FIR) {
            log_error("--filter-fft-size cannot be used with an explicit '--filter-type fir'.");
            return false;
        }
        if (config->dsp.filter.type_req != FILTER_TYPE_FFT) {
            log_debug("--filter-fft-size forces filter type to FFT.");
            config->dsp.filter.type_req = FILTER_TYPE_FFT;
        }
        int n = config->dsp.filter.args.fft_size;
        if (n <= 0 || ((n & (n - 1)) != 0)) {
            log_error("--filter-fft-size must be a positive power of two.");
            return false;
        }
    }

    if (config->dsp.audio_writer_path != NULL || config->dsp.mute_audio) {
        if (config->output.payload != PAYLOAD_AUDIO) {
            log_error("Options --audio-writer and --mute-audio can only be used with audio demodulation modules.");
            return false;
        }
    }

    if (!agc_validate_options(config)) return false;

    if (config->output_sample_rate.provided && config->preset_name) {
        log_error("Option --output-sample-rate cannot be used with --preset.");
        return false;
    }
    if (config->dsp.raw_passthrough) {
        if (config->dsp.filter.count > 0 || config->dsp.freq_shift_hz != 0.0f ||
            config->dsp.iq_correction.enable || config->dsp.dc_block.enable) {
            log_error("--raw-passthrough cannot be used with any other DSP options.");
            return false;
        }
        if (config->output.payload == PAYLOAD_AUDIO) {
            log_error("Option --raw-passthrough cannot be used with audio demodulation modules.");
            return false;
        }
    }

    return true;
}
