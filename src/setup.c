#define _POSIX_C_SOURCE 200809L

#include "setup.h"
#include "types.h"
#include "config.h"
#include "platform.h"
#include "utils.h"
#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#endif

// --- Function Implementations ---

/**
 * @brief Resolves input and output paths.
 */
bool resolve_file_paths(AppConfig *config) {
    if (!config) return false;
#ifdef _WIN32
    if (!config->input_filename_arg ||
        !get_absolute_path_windows(config->input_filename_arg,
                                   &config->effective_input_filename_w,
                                   &config->effective_input_filename_utf8)) {
        return false;
    }
    if (!config->output_to_stdout) {
        if (!config->output_filename_arg ||
            !get_absolute_path_windows(config->output_filename_arg,
                                       &config->effective_output_filename_w,
                                       &config->effective_output_filename_utf8)) {
             free_absolute_path_windows(&config->effective_input_filename_w, &config->effective_input_filename_utf8);
             return false;
        }
    }
#else
    if (!config->input_filename_arg) return false;
    config->effective_input_filename = config->input_filename_arg;
    config->effective_output_filename = config->output_filename_arg;
#endif
    return true;
}

/**
 * @brief Opens and validates the input WAV file.
 */
bool open_and_validate_input_file(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    const char* input_path_for_libsndfile = config->input_filename_arg;
#ifdef _WIN32
    if (config->effective_input_filename_utf8) {
        input_path_for_libsndfile = config->effective_input_filename_utf8;
    }
#endif

    memset(&resources->sfinfo, 0, sizeof(SF_INFO));
    resources->infile = sf_open(input_path_for_libsndfile, SFM_READ, &resources->sfinfo);
    if (!resources->infile) {
        fprintf(stderr, "Error opening input file %s: %s\n", input_path_for_libsndfile, sf_strerror(NULL));
        return false;
    }

    if (resources->sfinfo.channels != 2) {
        fprintf(stderr, "Error: Input file %s must have 2 channels (I/Q), but found %d.\n",
                input_path_for_libsndfile, resources->sfinfo.channels);
        sf_close(resources->infile);
        return false;
    }

    resources->input_format_subtype = (resources->sfinfo.format & SF_FORMAT_SUBMASK);
    if (resources->input_format_subtype == SF_FORMAT_PCM_16) {
        resources->input_bit_depth = 16;
        resources->input_bytes_per_sample = sizeof(int16_t);
    } else if (resources->input_format_subtype == SF_FORMAT_PCM_S8) {
        resources->input_bit_depth = 8;
        resources->input_bytes_per_sample = sizeof(int8_t);
    } else if (resources->input_format_subtype == SF_FORMAT_PCM_U8) {
         resources->input_bit_depth = 8;
         resources->input_bytes_per_sample = sizeof(uint8_t);
    } else {
        fprintf(stderr, "Error: Unsupported PCM format in %s. Only 8/16-bit Signed/Unsigned PCM is supported.\n",
                 input_path_for_libsndfile);
        sf_close(resources->infile);
        return false;
    }

    if (resources->sfinfo.samplerate <= 0) {
        fprintf(stderr, "Error: Invalid input sample rate (%d Hz) in %s.\n",
                resources->sfinfo.samplerate, input_path_for_libsndfile);
        sf_close(resources->infile);
        return false;
    }

    if (resources->sfinfo.frames == 0) {
        fprintf(stderr, "Error: Input file %s appears to be empty (0 frames).\n", input_path_for_libsndfile);
        sf_close(resources->infile);
        return false;
    }
    return true;
}

/**
 * @brief Calculates and validates the resampling ratio.
 */
bool calculate_and_validate_resample_ratio(AppConfig *config, AppResources *resources, float *out_ratio) {
     if (!config || !resources || !out_ratio) return false;

    double input_rate_d = (double)resources->sfinfo.samplerate;
    float r = (float)(config->target_rate / input_rate_d);

    if (!isfinite(r) || r < MIN_ACCEPTABLE_RATIO || r > MAX_ACCEPTABLE_RATIO) {
        fprintf(stderr, "Error: Calculated resampling ratio (%.6f) is invalid or outside acceptable range.\n", r);
        return false;
    }
    *out_ratio = r;
    return true;
}

/**
 * @brief Allocates all necessary processing buffer pools.
 */
bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    // Estimate max output samples per chunk with a safety margin
    double estimated_output = ceil((double)BUFFER_SIZE_SAMPLES * (double)resample_ratio);
    resources->max_out_samples = (unsigned int)estimated_output + 128;

    // Sanity check the calculated buffer size
    double upper_limit = 20.0 * (double)BUFFER_SIZE_SAMPLES * fmax(1.0, (double)resample_ratio);
    if (resources->max_out_samples > upper_limit || resources->max_out_samples > (UINT_MAX - 128)) {
         fprintf(stderr, "Error: Calculated output buffer size per item (%u samples) is unreasonable.\n", resources->max_out_samples);
         return false;
    }

    size_t raw_input_bytes_per_item = BUFFER_SIZE_SAMPLES * 2 * resources->input_bytes_per_sample;
    size_t complex_elements_per_input_item = BUFFER_SIZE_SAMPLES;
    size_t complex_elements_per_output_item = resources->max_out_samples;
    bool shift_requested = config->freq_shift_requested || config->set_center_frequency_target_hz;
    size_t complex_elements_per_shifted_item = shift_requested ? (config->shift_after_resample ? complex_elements_per_output_item : complex_elements_per_input_item) : 0;
    resources->output_bytes_per_sample_pair = (config->mode == MODE_CU8) ? (sizeof(uint8_t) * 2) : (sizeof(int16_t) * 2);
    size_t output_buffer_bytes_per_item = complex_elements_per_output_item * resources->output_bytes_per_sample_pair;

    // Allocate all buffer pools
    resources->work_item_pool = malloc(NUM_BUFFERS * sizeof(WorkItem));
    resources->raw_input_pool = malloc(NUM_BUFFERS * raw_input_bytes_per_item);
    resources->complex_scaled_pool = malloc(NUM_BUFFERS * complex_elements_per_input_item * sizeof(liquid_float_complex));
    if (shift_requested) {
        resources->complex_shifted_pool = malloc(NUM_BUFFERS * complex_elements_per_shifted_item * sizeof(liquid_float_complex));
    }
    resources->complex_resampled_pool = malloc(NUM_BUFFERS * complex_elements_per_output_item * sizeof(liquid_float_complex));
    resources->output_pool = malloc(NUM_BUFFERS * output_buffer_bytes_per_item);

    if (!resources->work_item_pool || !resources->raw_input_pool || !resources->complex_scaled_pool ||
        (shift_requested && !resources->complex_shifted_pool) || !resources->complex_resampled_pool || !resources->output_pool) {
        fprintf(stderr, "Error: Failed to allocate one or more processing buffer pools.\n");
        return false;
    }
    return true;
}

/**
 * @brief Creates and initializes the liquid-dsp components.
 */
bool create_dsp_components(AppConfig *config, AppResources *resources, float resample_ratio) {
    if (!config || !resources) return false;

    double required_shift_hz = 0.0;
    if (config->set_center_frequency_target_hz) {
        if (!resources->sdr_info.center_freq_hz_present) {
            fprintf(stderr, "Error: --target-freq provided, but input file lacks center frequency metadata.\n");
            return false;
        }
        required_shift_hz = resources->sdr_info.center_freq_hz - config->center_frequency_target_hz;
    } else if (config->freq_shift_requested) {
        required_shift_hz = config->freq_shift_hz;
    }
    resources->actual_nco_shift_hz = required_shift_hz;

    // Create NCO (shifter) if needed
    if (fabs(resources->actual_nco_shift_hz) > 1e-9) {
        double rate_for_nco_setup = config->shift_after_resample ? config->target_rate : (double)resources->sfinfo.samplerate;
        if (fabs(resources->actual_nco_shift_hz) > (SHIFT_FACTOR_LIMIT * rate_for_nco_setup)) {
            fprintf(stderr, "Error: Requested frequency shift %.2f Hz exceeds sanity limit.\n", resources->actual_nco_shift_hz);
            return false;
        }
        resources->shifter_nco = nco_crcf_create(LIQUID_NCO);
        if (!resources->shifter_nco) {
            fprintf(stderr,"Error: Failed to create NCO (frequency shifter).\n");
            return false;
        }
        float nco_freq_rad_per_sample = (float)(2.0 * M_PI * fabs(resources->actual_nco_shift_hz) / rate_for_nco_setup);
        nco_crcf_set_frequency(resources->shifter_nco, nco_freq_rad_per_sample);
    }

    // Create resampler
    resources->resampler = msresamp_crcf_create(resample_ratio, STOPBAND_ATTENUATION_DB);
    if (!resources->resampler) {
        fprintf(stderr, "Error: Failed to create liquid-dsp resampler object.\n");
        return false;
    }
    return true;
}

/**
 * @brief Prints the full configuration summary to stderr.
 */
void print_configuration_summary(const AppConfig *config, const AppResources *resources, float resample_ratio) {
    if (!config || !resources) return;
    (void)resample_ratio; // Unused, but kept for API consistency

    const char* input_path_for_messages = config->input_filename_arg;
    const char* output_path_for_messages = config->output_filename_arg;
#ifdef _WIN32
    if (config->effective_input_filename_utf8) input_path_for_messages = config->effective_input_filename_utf8;
    if (config->effective_output_filename_utf8) output_path_for_messages = config->effective_output_filename_utf8;
#endif

    long long input_file_size = -1LL;
    char input_size_str_buf[40];
#ifdef _WIN32
    struct __stat64 stat_buf64;
    if (_stat64(input_path_for_messages, &stat_buf64) == 0) input_file_size = stat_buf64.st_size;
#else
    struct stat stat_buf;
    if (stat(input_path_for_messages, &stat_buf) == 0) input_file_size = stat_buf.st_size;
#endif
    format_file_size(input_file_size, input_size_str_buf, sizeof(input_size_str_buf));

    fprintf(stderr, "---Input Details---\n");
    fprintf(stderr, "Input File:        %s\n", input_path_for_messages);
    const char *format_str = (resources->input_format_subtype == SF_FORMAT_PCM_16) ? "16-bit Signed PCM" :
                             (resources->input_format_subtype == SF_FORMAT_PCM_S8) ? "8-bit Signed PCM" : "8-bit Unsigned PCM";
    fprintf(stderr, "Input Format:      %s\n", format_str);
    fprintf(stderr, "Input Rate:        %.1f Hz\n", (double)resources->sfinfo.samplerate);
    fprintf(stderr, "Input File Size:   %s\n", input_size_str_buf);

    if (resources->sdr_info_present) {
        if (resources->sdr_info.timestamp_unix_present) {
            char time_buf[64];
            struct tm time_info;
            #ifdef _WIN32
            if (gmtime_s(&time_info, &resources->sdr_info.timestamp_unix) == 0) {
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", &time_info);
                fprintf(stderr, "Timestamp:         %s\n", time_buf);
            }
            #else
            if (gmtime_r(&resources->sdr_info.timestamp_unix, &time_info)) {
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", &time_info);
                fprintf(stderr, "Timestamp:         %s\n", time_buf);
            }
            #endif
        } else if (resources->sdr_info.timestamp_str_present) {
            fprintf(stderr, "Timestamp:         %s\n", resources->sdr_info.timestamp_str);
        }
        if (resources->sdr_info.center_freq_hz_present) {
            fprintf(stderr, "Center Frequency:  %.6f MHz\n", resources->sdr_info.center_freq_hz / 1e6);
        }
        if (resources->sdr_info.software_name_present) {
             fprintf(stderr, "SDR Software:      %s %s\n", resources->sdr_info.software_name,
                     resources->sdr_info.software_version_present ? resources->sdr_info.software_version : "");
        }
        if (resources->sdr_info.radio_model_present) {
            fprintf(stderr, "Radio Model:       %s\n", resources->sdr_info.radio_model);
        }
    }

    fprintf(stderr, "---Output Details---\n");
    const char* mode_str = (config->mode == MODE_CU8) ? "cu8 (Unsigned 8-bit Complex)" :
                           (config->mode == MODE_CS16_AM) ? "cs16_am (Signed 16-bit Complex)" : "cs16_fm (Signed 16-bit Complex)";
    fprintf(stderr, "Output Mode:       %s\n", mode_str);
    fprintf(stderr, "Output Rate:       %.1f Hz\n", config->target_rate);
    fprintf(stderr, "Scale Factor:      %.5f\n", config->scale_value);
    if (config->set_center_frequency_target_hz) {
        fprintf(stderr, "Target Frequency:  %.6f MHz\n", config->center_frequency_target_hz / 1e6);
    }
    fprintf(stderr, "Frequency Shift:   %+.2f Hz%s\n",
            resources->actual_nco_shift_hz,
            config->shift_after_resample ? " (Applied After Resampling)" : " (Applied Before Resampling)");
    fprintf(stderr, "Output File:       %s\n", config->output_to_stdout ? "stdout" : output_path_for_messages);
}

/**
 * @brief Checks for Nyquist warning and prompts user if necessary.
 */
bool check_nyquist_warning(const AppConfig *config, const AppResources *resources) {
     if (!config || !resources || config->output_to_stdout || fabs(resources->actual_nco_shift_hz) < 1e-9) {
        return true;
     }

    double rate_for_nyquist_check = config->shift_after_resample ? config->target_rate : (double)resources->sfinfo.samplerate;
    double nyquist_freq = rate_for_nyquist_check / 2.0;

    if (fabs(resources->actual_nco_shift_hz) > nyquist_freq) {
        fprintf(stderr, "\nWarning: Required frequency shift %.2f Hz exceeds the Nyquist frequency %.2f Hz.\n",
                resources->actual_nco_shift_hz, nyquist_freq);
        fprintf(stderr, "         This may cause aliasing and corrupt the signal.\n");

        int response;
        do {
            fprintf(stderr, "Continue anyway? (y/n): ");
            response = getchar();
            if (response == EOF) {
                 fprintf(stderr, "\nEOF detected. Cancelling.\n");
                 return false;
            }
            clear_stdin_buffer();
            response = tolower(response);
            if (response == 'n') {
                fprintf(stderr, "\nOperation cancelled by user.\n");
                return false;
            }
        } while (response != 'y');
    }
    return true;
}

/**
 * @brief Prepares the output stream (file or stdout).
 */
bool prepare_output_stream(AppConfig *config, AppResources *resources) {
     if (!config || !resources) return false;

    if (config->output_to_stdout) {
        resources->outfile = stdout;
        #ifdef _WIN32
        if (!set_stdout_binary()) return false;
        #endif
        return true;
    }

    // --- File Output Handling ---
#ifdef _WIN32
    const wchar_t* out_path_w = config->effective_output_filename_w;
    const char* out_path_msg = config->effective_output_filename_utf8;
    resources->h_outfile = CreateFileW(out_path_w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (resources->h_outfile == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) {
            fprintf(stderr, "\nOutput file %s exists.\nOverwrite? (y/n): ", out_path_msg);
            int response = tolower(getchar());
            clear_stdin_buffer();
            if (response == 'y') {
                resources->h_outfile = CreateFileW(out_path_w, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            } else {
                fprintf(stderr, "\nOperation cancelled by user.\n");
                return false;
            }
        }
        if (resources->h_outfile == INVALID_HANDLE_VALUE) {
            print_win_error("CreateFileW", GetLastError());
            return false;
        }
    }
    int fd = _open_osfhandle((intptr_t)resources->h_outfile, _O_WRONLY | _O_BINARY);
    if (fd == -1) {
        perror("Error associating Windows handle with file descriptor");
        CloseHandle(resources->h_outfile);
        return false;
    }
    resources->outfile = _fdopen(fd, "wb");
    if (!resources->outfile) {
        perror("Error converting file descriptor to FILE*");
        _close(fd);
        return false;
    }
#else // POSIX
    const char* out_path = config->effective_output_filename;
    if (access(out_path, F_OK) == 0) {
        fprintf(stderr, "\nOutput file %s exists.\nOverwrite? (y/n): ", out_path);
        int response = tolower(getchar());
        clear_stdin_buffer();
        if (response != 'y') {
            fprintf(stderr, "\nOperation cancelled by user.\n");
            return false;
        }
    }
    resources->outfile = fopen(out_path, "wb");
    if (!resources->outfile) {
        fprintf(stderr, "\nError opening output file %s: %s\n", out_path, strerror(errno));
        return false;
    }
#endif
    fprintf(stderr, "\nStarting Resample...\n\n");
    return true;
}
